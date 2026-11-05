/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <assert.h>
#if HAVE_ERR
# include <err.h>
#endif
#if !HAVE_SOCK_NONBLOCK
# include <fcntl.h>
#endif
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if HAVE_SCAN_SCALED
# include <util.h>
#endif

#include "extern.h"

/*
 * The iOS app has no real fork: a local transfer's server is a thread of this
 * process, parsing its own arguments alongside the client. And no process exit
 * ends a run there, so the next run on a thread finds what the last one left.
 * So what the options set is per thread, and main() starts by clearing it.
 */
_Thread_local int verbose;
_Thread_local int poll_contimeout;
_Thread_local int poll_timeout;

/*
 * A remote host is has a colon before the first path separator.
 * This works for rsh remote hosts (host:/foo/bar), implicit rsync
 * remote hosts (host::/foo/bar), and explicit (rsync://host/foo).
 * Return zero if local, non-zero if remote.
 */
static int
fargs_is_remote(const char *v)
{
	size_t	 pos;

	pos = strcspn(v, ":/");
	return v[pos] == ':';
}

/*
 * Test whether a remote host is specifically an rsync daemon.
 * Return zero if not, non-zero if so.
 */
static int
fargs_is_daemon(const char *v)
{
	size_t	 pos;

	if (strncasecmp(v, "rsync://", 8) == 0)
		return 1;

	pos = strcspn(v, ":/");
	return v[pos] == ':' && v[pos + 1] == ':';
}

/*
 * Take the command-line filenames (e.g., rsync foo/ bar/ baz/) and
 * determine our operating mode.
 * For example, if the first argument is a remote file, this means that
 * we're going to transfer from the remote to the local.
 * We also make sure that the arguments are consistent, that is, if
 * we're going to transfer from the local to the remote, that no
 * filenames for the local transfer indicate remote hosts.
 * Always returns the parsed and sanitised options.
 */
static struct fargs *
fargs_parse(size_t argc, char *argv[], struct opts *opts)
{
	struct fargs	*f = NULL;
	char		*cp, *ccp;
	size_t		 i, j, len = 0;

	/* Allocations. */

	if ((f = calloc(1, sizeof(struct fargs))) == NULL)
		err(ERR_NOMEM, NULL);

	f->sourcesz = argc - 1;
	if ((f->sources = calloc(f->sourcesz, sizeof(char *))) == NULL)
		err(ERR_NOMEM, NULL);

	for (i = 0; i < argc - 1; i++)
		if ((f->sources[i] = strdup(argv[i])) == NULL)
			err(ERR_NOMEM, NULL);

	if ((f->sink = strdup(argv[i])) == NULL)
		err(ERR_NOMEM, NULL);
	if (f->sink[0] == '\0')
		errx(ERR_SYNTAX, "empty destination");

	/*
	 * Test files for its locality.
	 * If the last is a remote host, then we're sending from the
	 * local to the remote host ("sender" mode).
	 * If the first, remote to local ("receiver" mode).
	 * If neither, a local transfer in sender style.
	 */

	f->mode = FARGS_SENDER;

	if (fargs_is_remote(f->sink)) {
		f->mode = FARGS_SENDER;
		if ((f->host = strdup(f->sink)) == NULL)
			err(ERR_NOMEM, NULL);
	}

	if (fargs_is_remote(f->sources[0])) {
		if (f->host != NULL)
			errx(ERR_SYNTAX, "both source and destination "
			    "cannot be remote files");
		f->mode = FARGS_RECEIVER;
		if ((f->host = strdup(f->sources[0])) == NULL)
			err(ERR_NOMEM, NULL);
	}

	if (f->host != NULL) {
		if (strncasecmp(f->host, "rsync://", 8) == 0) {
			/* rsync://host[:port]/module[/path] */
			f->remote = 1;
			len = strlen(f->host) - 8 + 1;
			memmove(f->host, f->host + 8, len);
			if ((cp = strchr(f->host, '/')) == NULL)
				errx(ERR_SYNTAX,
				    "rsync protocol requires a module name");
			*cp++ = '\0';
			f->module = cp;
			if ((cp = strchr(f->module, '/')) != NULL)
				*cp = '\0';
			if ((cp = strchr(f->host, ':')) != NULL) {
				/* host:port --> extract port */
				*cp++ = '\0';
				opts->port = cp;
			}
		} else {
			/* host:[/path] */
			cp = strchr(f->host, ':');
			assert(cp != NULL);
			*cp++ = '\0';
			if (*cp == ':') {
				/* host::module[/path] */
				f->remote = 1;
				f->module = ++cp;
				cp = strchr(f->module, '/');
				if (cp != NULL)
					*cp = '\0';
			}
		}
		if ((len = strlen(f->host)) == 0)
			errx(ERR_SYNTAX, "empty remote host");
		if (f->remote && strlen(f->module) == 0)
			errx(ERR_SYNTAX, "empty remote module");
	}

	/* Make sure we have the same "hostspec" for all files. */

	if (!f->remote) {
		if (f->mode == FARGS_SENDER)
			for (i = 0; i < f->sourcesz; i++) {
				if (!fargs_is_remote(f->sources[i]))
					continue;
				errx(ERR_SYNTAX,
				    "remote file in list of local sources: %s",
				    f->sources[i]);
			}
		if (f->mode == FARGS_RECEIVER)
			for (i = 0; i < f->sourcesz; i++) {
				if (fargs_is_remote(f->sources[i]) &&
				    !fargs_is_daemon(f->sources[i]))
					continue;
				if (fargs_is_daemon(f->sources[i]))
					errx(ERR_SYNTAX,
					    "remote daemon in list of remote "
					    "sources: %s", f->sources[i]);
				errx(ERR_SYNTAX, "local file in list of "
				    "remote sources: %s", f->sources[i]);
			}
	} else if (f->mode == FARGS_RECEIVER) {
		for (i = 0; i < f->sourcesz; i++) {
			if (fargs_is_daemon(f->sources[i]))
				continue;
			errx(ERR_SYNTAX, "non-remote daemon file "
				"in list of remote daemon sources: "
				"%s", f->sources[i]);
		}
	} else {
		/*
		 * Pushing to a remote daemon: the lone remote endpoint is
		 * f->sink (already used above to populate f->host/f->module),
		 * so every source must be a plain local path instead.
		 */
		for (i = 0; i < f->sourcesz; i++) {
			if (!fargs_is_remote(f->sources[i]))
				continue;
			errx(ERR_SYNTAX,
			    "remote file in list of local sources: %s",
			    f->sources[i]);
		}
	}

	/*
	 * If we're not remote and a sender, strip our hostname.
	 * Then exit if we're a sender or a local connection.
	 */

	if (!f->remote) {
		if (f->host == NULL)
			return f;
		if (f->mode == FARGS_SENDER) {
			assert(f->host != NULL);
			assert(len > 0);
			j = strlen(f->sink);
			memmove(f->sink, f->sink + len + 1, j - len);
			return f;
		} else if (f->mode != FARGS_RECEIVER)
			return f;
	}

	if (f->remote && f->mode == FARGS_SENDER) {
		/*
		 * Pushing to a remote daemon: f->sources[] are all local
		 * (validated above) and left untouched -- only f->sink
		 * carries the "rsync://host[:port]/module/path" or
		 * "host::module/path" prefix that needs stripping down to
		 * a bare "module/path", mirroring the per-source stripping
		 * done below for the pull/receiver direction.
		 */
		assert(f->host != NULL);
		assert(len > 0);
		cp = f->sink;
		j = strlen(cp);
		if (strncasecmp(cp, "rsync://", 8) == 0) {
			size_t module_offset = len;
			cp += 8;
			if ((ccp = strchr(cp, ':')) != NULL) {
				*ccp = '\0';
				module_offset += strcspn(ccp + 1, "/") + 1;
			}
			memmove(f->sink, f->sink + module_offset + 8 + 1,
			    j - module_offset - 8);
		} else {
			/* host::module/path */
			memmove(f->sink, f->sink + len + 2, j - len - 1);
		}
		return f;
	}

	/*
	 * Now strip the hostnames from the remote host.
	 *   rsync://host/module/path -> module/path
	 *   host::module/path -> module/path
	 *   host:path -> path
	 * Also make sure that the remote hosts are the same.
	 */

	assert(f->host != NULL);
	assert(len > 0);

	for (i = 0; i < f->sourcesz; i++) {
		cp = f->sources[i];
		j = strlen(cp);
		if (f->remote &&
		    strncasecmp(cp, "rsync://", 8) == 0) {
			/* rsync://host[:port]/path */
			size_t module_offset = len;
			cp += 8;
			/* skip :port */
			if ((ccp = strchr(cp, ':')) != NULL) {
				*ccp = '\0';
				module_offset += strcspn(ccp + 1, "/") + 1;
			}
			if (strncmp(cp, f->host, len) ||
			    (cp[len] != '/' && cp[len] != '\0'))
				errx(ERR_SYNTAX, "different remote host: %s",
				    f->sources[i]);
			memmove(f->sources[i],
				f->sources[i] + module_offset + 8 + 1,
				j - module_offset - 8);
		} else if (f->remote && strncmp(cp, "::", 2) == 0) {
			/* ::path */
			memmove(f->sources[i],
				f->sources[i] + 2, j - 1);
		} else if (f->remote) {
			/* host::path */
			if (strncmp(cp, f->host, len) ||
			    (cp[len] != ':' && cp[len] != '\0'))
				errx(ERR_SYNTAX, "different remote host: %s",
				    f->sources[i]);
			memmove(f->sources[i], f->sources[i] + len + 2,
			    j - len - 1);
		} else if (cp[0] == ':') {
			/* :path */
			memmove(f->sources[i], f->sources[i] + 1, j);
		} else {
			/* host:path */
			if (strncmp(cp, f->host, len) ||
			    (cp[len] != ':' && cp[len] != '\0'))
				errx(ERR_SYNTAX, "different remote host: %s",
				    f->sources[i]);
			memmove(f->sources[i],
				f->sources[i] + len + 1, j - len);
		}
	}

	return f;
}

static _Thread_local struct opts	 opts;
/* lopts cannot point at thread-local opts: it points here and main() repoints a copy. */
static struct opts	 opts_layout;

#define OP_ADDRESS	1000
#define OP_PORT		1001
#define OP_RSYNCPATH	1002
#define OP_TIMEOUT	1003
#define OP_EXCLUDE	1005
#define OP_INCLUDE	1006
#define OP_EXCLUDE_FROM	1007
#define OP_INCLUDE_FROM	1008
#define OP_COMP_DEST	1009
#define OP_COPY_DEST	1010
#define OP_LINK_DEST	1011
#define OP_MAX_SIZE	1012
#define OP_MIN_SIZE	1013
#define OP_CONTIMEOUT	1014
#define OP_VERBOSE	1015
#define OP_NO_VERBOSE	1016

const struct option	 lopts[] = {
    { "address",	required_argument, NULL,		OP_ADDRESS },
    { "archive",	no_argument,	NULL,			'a' },
    { "checksum",	no_argument,	NULL,			'c' },
    { "compare-dest",	required_argument, NULL,		OP_COMP_DEST },
    { "copy-dest",	required_argument, NULL,		OP_COPY_DEST },
    { "link-dest",	required_argument, NULL,		OP_LINK_DEST },
    { "compress",	no_argument,	NULL,			'z' },
    { "contimeout",	required_argument, NULL,		OP_CONTIMEOUT },
    { "del",		no_argument,	&opts_layout.del,		1 },
    { "delete",		no_argument,	&opts_layout.del,		1 },
    { "devices",	no_argument,	&opts_layout.devices,		1 },
    { "no-devices",	no_argument,	&opts_layout.devices,		0 },
    { "dry-run",	no_argument,	&opts_layout.dry_run,		1 },
    { "exclude",	required_argument, NULL,		OP_EXCLUDE },
    { "exclude-from",	required_argument, NULL,		OP_EXCLUDE_FROM },
    { "group",		no_argument,	&opts_layout.preserve_gids,	1 },
    { "no-group",	no_argument,	&opts_layout.preserve_gids,	0 },
    { "help",		no_argument,	NULL,			'h' },
    { "ignore-times",	no_argument,	NULL,			'I' },
    { "include",	required_argument, NULL,		OP_INCLUDE },
    { "include-from",	required_argument, NULL,		OP_INCLUDE_FROM },
    { "links",		no_argument,	&opts_layout.preserve_links,	1 },
    { "max-size",	required_argument, NULL,		OP_MAX_SIZE },
    { "min-size",	required_argument, NULL,		OP_MIN_SIZE },
    { "no-links",	no_argument,	&opts_layout.preserve_links,	0 },
    { "no-motd",	no_argument,	&opts_layout.no_motd,		1 },
    { "numeric-ids",	no_argument,	&opts_layout.numeric_ids,	1 },
    { "omit-dir-times",	no_argument,	&opts_layout.ignore_dir_times,	1 },
    { "no-O",		no_argument,	&opts_layout.ignore_dir_times,	0 },
    { "no-omit-dir-times",	no_argument,	&opts_layout.ignore_dir_times, 0 },
    { "omit-link-times",	no_argument,	&opts_layout.ignore_link_times, 1 },
    { "no-J",		no_argument,	&opts_layout.ignore_link_times, 0 },
    { "no-omit-link-times",	no_argument,	&opts_layout.ignore_link_times, 0 },
    { "owner",		no_argument,	&opts_layout.preserve_uids,	1 },
    { "no-owner",	no_argument,	&opts_layout.preserve_uids,	0 },
    { "perms",		no_argument,	&opts_layout.preserve_perms,	1 },
    { "no-perms",	no_argument,	&opts_layout.preserve_perms,	0 },
    { "port",		required_argument, NULL,		OP_PORT },
    { "progress",	no_argument,	&opts_layout.progress,		1 },
    { "recursive",	no_argument,	&opts_layout.recursive,	1 },
    { "no-recursive",	no_argument,	&opts_layout.recursive,	0 },
    { "rsh",		required_argument, NULL,		'e' },
    { "rsync-path",	required_argument, NULL,		OP_RSYNCPATH },
    { "sender",		no_argument,	&opts_layout.sender,		1 },
    { "server",		no_argument,	&opts_layout.server,		1 },
    { "size-only",	no_argument,	&opts_layout.size_only,	1 },
    { "specials",	no_argument,	&opts_layout.specials,		1 },
#if 0
    { "sync-file",	required_argument, NULL,		6 },
#endif
    { "no-specials",	no_argument,	&opts_layout.specials,		0 },
    { "timeout",	required_argument, NULL,		OP_TIMEOUT },
    { "times",		no_argument,	&opts_layout.preserve_times,	1 },
    { "no-times",	no_argument,	&opts_layout.preserve_times,	0 },
    { "update",		no_argument,	&opts_layout.update,		1 },
    { "verbose",	no_argument,	NULL,			OP_VERBOSE },
    { "no-verbose",	no_argument,	NULL,			OP_NO_VERBOSE },
    { "version",	no_argument,	NULL,			'V' },
    { NULL,		0,		NULL,			0 }
};

#ifdef PSCAL_TARGET_IOS
/*
 * In the app a relative path resolves against the command's own vproc only
 * through some calls, and the server thread is a different one again.  Make the
 * local paths absolute while running as the command itself: it is still the
 * app's virtual view, so messages read as the user would expect.
 */
static char *
fargs_absolute_path(char *path, int source)
{
	char		 cwd[PATH_MAX], *absolute;
	const char	*base;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		err(ERR_FILE_IO, "getcwd");

	/*
	 * With "." or ".." a source's name is no use: they mean the contents.
	 * A destination's slash would ask for a directory, so it gets none.
	 */
	base = strrchr(path, '/');
	base = base != NULL ? base + 1 : path;
	if (asprintf(&absolute, "%s%s%s%s", path[0] == '/' ? "" : cwd,
	    path[0] == '/' ? "" : "/", path,
	    source && (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) ?
	    "/" : "") == -1)
		err(ERR_NOMEM, NULL);
	free(path);
	return absolute;
}

static void
fargs_absolute_paths(struct fargs *f)
{
	size_t	 i;

	if (f->host == NULL || f->mode == FARGS_SENDER)
		for (i = 0; i < f->sourcesz; i++)
			f->sources[i] = fargs_absolute_path(f->sources[i], 1);
	if (f->host == NULL || f->mode == FARGS_RECEIVER)
		f->sink = fargs_absolute_path(f->sink, 0);
}
#endif

int
main(int argc, char *argv[])
{
	pid_t		 child, reaped;
	long long	 tmpint;
	int		 fds[2], sd = -1, rc, c, st, i, lidx;
	int		 timeout_explicit = 0;
	size_t		 basedir_cnt = 0;
	struct option	 run_lopts[sizeof(lopts) / sizeof(lopts[0])];
	struct sess	 sess;
	struct fargs	*fargs;
	char		**args;
	const char	*errstr;

	/* Global pledge. */

	if (pledge("stdio unix rpath wpath cpath dpath inet fattr chown dns getpw proc exec unveil",
	    NULL) == -1)
		err(ERR_IPC, "pledge");

	memset(&opts, 0, sizeof(opts));
	verbose = poll_contimeout = poll_timeout = 0;
	rules_reset();

	opts.max_size = opts.min_size = -1;

	for (i = 0; i < (int)(sizeof(lopts) / sizeof(lopts[0])); i++) {
		run_lopts[i] = lopts[i];
		if (lopts[i].flag != NULL)
			run_lopts[i].flag = (int *)((char *)&opts +
			    ((char *)lopts[i].flag - (char *)&opts_layout));
	}

	while ((c = getopt_long(argc, argv, "aDe:cghIJlnOoprtuVvxz",
	    run_lopts, &lidx)) != -1) {
		switch (c) {
		case 'D':
			opts.devices = 1;
			opts.specials = 1;
			break;
		case 'a':
			opts.recursive = 1;
			opts.preserve_links = 1;
			opts.preserve_perms = 1;
			opts.preserve_times = 1;
			opts.preserve_gids = 1;
			opts.preserve_uids = 1;
			opts.devices = 1;
			opts.specials = 1;
			break;
		case 'c':
			opts.checksum = 1;
			break;
		case 'e':
			opts.ssh_prog = optarg;
			break;
		case 'g':
			opts.preserve_gids = 1;
			break;
		case 'I':
			opts.ignore_times = 1;
			break;
		case 'J':
			opts.ignore_link_times = 1;
			break;
		case 'l':
			opts.preserve_links = 1;
			break;
		case 'n':
			opts.dry_run = 1;
			break;
		case 'O':
			opts.ignore_dir_times = 1;
			break;
		case 'o':
			opts.preserve_uids = 1;
			break;
		case 'p':
			opts.preserve_perms = 1;
			break;
		case 'r':
			opts.recursive = 1;
			break;
		case 't':
			opts.preserve_times = 1;
			break;
		case 'u':
			opts.update = 1;
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			fprintf(stderr, "openrsync: protocol version %u\n",
			    RSYNC_PROTOCOL);
			exit(0);
		case 'x':
			opts.one_file_system++;
			break;
		case 'z':
			/*
			 * openrsync does not currently implement rsync-protocol
			 * payload compression. Preserve CLI compatibility by
			 * requesting transport compression from ssh.
			 */
			opts.compress = 1;
			break;
		case 0:
			/* Non-NULL flag values (e.g., --sender). */
			break;
		case OP_VERBOSE:
			verbose = 1;
			break;
		case OP_NO_VERBOSE:
			verbose = 0;
			break;
#if 0
		case 6:
			opts.syncfile = optarg;
			break;
#endif
		case OP_ADDRESS:
			opts.address = optarg;
			break;
		case OP_CONTIMEOUT:
			poll_contimeout = strtonum(optarg, 0, 60*60, &errstr);
			if (errstr != NULL)
				errx(ERR_SYNTAX, "timeout is %s: %s",
				    errstr, optarg);
			break;
		case OP_PORT:
			opts.port = optarg;
			break;
		case OP_RSYNCPATH:
			opts.rsync_path = optarg;
			break;
		case OP_TIMEOUT:
			poll_timeout = strtonum(optarg, 0, 60*60, &errstr);
			if (errstr != NULL)
				errx(ERR_SYNTAX, "timeout is %s: %s",
				    errstr, optarg);
			timeout_explicit = 1;
			break;
		case OP_EXCLUDE:
			if (parse_rule(optarg, RULE_EXCLUDE) == -1)
				errx(ERR_SYNTAX, "syntax error in exclude: %s",
				    optarg);
			break;
		case OP_INCLUDE:
			if (parse_rule(optarg, RULE_INCLUDE) == -1)
				errx(ERR_SYNTAX, "syntax error in include: %s",
				    optarg);
			break;
		case OP_EXCLUDE_FROM:
			parse_file(optarg, RULE_EXCLUDE);
			break;
		case OP_INCLUDE_FROM:
			parse_file(optarg, RULE_INCLUDE);
			break;
		case OP_COMP_DEST:
			if (opts.alt_base_mode !=0 &&
			    opts.alt_base_mode != BASE_MODE_COMPARE) {
				errx(1, "option --%s conflicts with %s",
				    lopts[lidx].name,
				    alt_base_mode(opts.alt_base_mode));
			}
			opts.alt_base_mode = BASE_MODE_COMPARE;
			goto basedir;
		case OP_COPY_DEST:
			if (opts.alt_base_mode !=0 &&
			    opts.alt_base_mode != BASE_MODE_COPY) {
				errx(1, "option --%s conflicts with %s",
				    lopts[lidx].name,
				    alt_base_mode(opts.alt_base_mode));
			}
			opts.alt_base_mode = BASE_MODE_COPY;
			goto basedir;
		case OP_LINK_DEST:
			if (opts.alt_base_mode !=0 &&
			    opts.alt_base_mode != BASE_MODE_LINK) {
				errx(1, "option --%s conflicts with %s",
				    lopts[lidx].name,
				    alt_base_mode(opts.alt_base_mode));
			}
			opts.alt_base_mode = BASE_MODE_LINK;

basedir:
			if (basedir_cnt >= MAX_BASEDIR)
				errx(1, "too many --%s directories specified",
				    lopts[lidx].name);
			opts.basedir[basedir_cnt++] = optarg;
			break;
		case OP_MAX_SIZE:
			if (scan_scaled(optarg, &tmpint) == -1)
				err(1, "bad max-size");
			opts.max_size = tmpint;
			break;
		case OP_MIN_SIZE:
			if (scan_scaled(optarg, &tmpint) == -1)
				err(1, "bad min-size");
			opts.min_size = tmpint;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	argc -= optind;
	argv += optind;

	/* FIXME: reference implementation rsync accepts this. */

	if (argc < 2)
		goto usage;

	if (opts.port == NULL)
		opts.port = (char *)"rsync";

	/* by default and for --contimeout=0 disable poll_contimeout */
	if (poll_contimeout == 0)
		poll_contimeout = -1;
	else
		poll_contimeout *= 1000;

	/*
	 * -c/--checksum against a peer that isn't openrsync/smallclue can
	 * hang indefinitely instead of failing: the foreign peer's own
	 * --checksum implementation may expect a whole-file-checksum wire
	 * exchange this fork doesn't produce, leaving both sides blocked in
	 * poll() forever (see the long comment on -c in fargs.c). -z avoids
	 * this because its incompatible wire format makes a foreign peer
	 * fail fast on its own; -c's framing is otherwise compatible enough
	 * that nothing naturally breaks the deadlock. An explicit
	 * --timeout=N (including --timeout=0 to force "never") always wins;
	 * this only fills in a bounded default when the user didn't ask for
	 * one at all, turning a silent indefinite hang into a diagnosable
	 * failure after PSCAL_CHECKSUM_DEFAULT_TIMEOUT seconds.
	 */
#define PSCAL_CHECKSUM_DEFAULT_TIMEOUT 60
	if (opts.checksum && !timeout_explicit && poll_timeout == 0)
		poll_timeout = PSCAL_CHECKSUM_DEFAULT_TIMEOUT;

	/* by default and for --timeout=0 disable poll_timeout */
	if (poll_timeout == 0)
		poll_timeout = -1;
	else
		poll_timeout *= 1000;

	/*
	 * This is what happens when we're started with the "hidden"
	 * --server option, which is invoked for the rsync on the remote
	 * host by the parent.
	 */

	if (opts.server) {
		rc = rsync_server(&opts, (size_t)argc, argv);
		rules_reset();
		exit(rc);
	}

	/*
	 * Now we know that we're the client on the local machine
	 * invoking rsync(1).
	 * At this point, we need to start the client and server
	 * initiation logic.
	 * The client is what we continue running on this host; the
	 * server is what we'll use to connect to the remote and
	 * invoke rsync with the --server option.
	 */

	fargs = fargs_parse(argc, argv, &opts);
	assert(fargs != NULL);
#ifdef PSCAL_TARGET_IOS
	fargs_absolute_paths(fargs);
#endif

	/*
	 * If we're contacting an rsync:// daemon, then we don't need to
	 * fork, because we won't start a server ourselves.
	 * Route directly into the socket code, unless a remote shell
	 * has explicitly been specified.
	 */

	if (fargs->remote && opts.ssh_prog == NULL) {
		if ((rc = rsync_connect(&opts, &sd, fargs)) == 0) {
			rc = rsync_socket(&opts, sd, fargs);
			close(sd);
		}
		exit(rc);
	}

	/* Drop the dns/inet possibility. */

	if (pledge("stdio unix rpath wpath cpath dpath fattr chown getpw proc exec unveil",
	    NULL) == -1)
		err(ERR_IPC, "pledge");

	/* Create a bidirectional socket and start our child. */

#if HAVE_SOCK_NONBLOCK
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == -1)
		err(ERR_IPC, "socketpair");
#else
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1)
		err(ERR_IPC, "socketpair");
	if (fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK) == -1)
		err(ERR_IPC, "fcntl");
	if (fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK) == -1)
		err(ERR_IPC, "fcntl");
#endif

	/*
	 * Before the fork: err() from a simulated child (iOS) would unwind the
	 * thread that is also the parent.  Past this point the child only uses
	 * _exit, which ends just the child there.
	 */
	memset(&sess, 0, sizeof(struct sess));
	sess.opts = &opts;

	args = fargs_cmdline(&sess, fargs, NULL);

	for (i = 0; args[i] != NULL; i++)
		LOG2("exec[%d] = %s", i, args[i]);

	switch ((child = fork())) {
	case -1:
		err(ERR_IPC, "fork");
	case 0:
		close(fds[0]);
		if (pledge("stdio exec", NULL) == -1) {
			warn("pledge");
			_exit(ERR_IPC);
		}

		/* Make sure the child's stdin is from the sender. */
		if (dup2(fds[1], STDIN_FILENO) == -1 ||
		    dup2(fds[1], STDOUT_FILENO) == -1) {
			warn("dup2");
			_exit(ERR_IPC);
		}
		execvp(args[0], args);
		warn("%s", args[0]);
		_exit(ERR_IPC);
		/* NOTREACHED */
	default:
		close(fds[1]);
		if (!fargs->remote)
			rc = rsync_client(&opts, fds[0], fargs);
		else
			rc = rsync_socket(&opts, fds[0], fargs);
		break;
	}

	/*
	 * A server that has our good-bye ends by itself, so give it a moment
	 * before closing.  The rsync in macOS exits 14 if it sees the end of
	 * its input first, with everything already transferred.  Bounded, as
	 * one that doesn't end needs the close to.
	 */
	reaped = 0;
	for (i = 0; rc == 0 && reaped == 0 && i < 40; i++) {
		if ((reaped = waitpid(child, &st, WNOHANG)) == 0)
			usleep(50000);
	}

	close(fds[0]);

	if (reaped == 0)
		reaped = waitpid(child, &st, 0);
	if (reaped == -1)
		err(ERR_WAITPID, "waitpid");

	/*
	 * If we don't already have an error (rc == 0), then inherit the
	 * error code of rsync_server() if it has exited.
	 * If it hasn't exited, it overrides our return value.
	 */

	if (rc == 0) {
		if (WIFEXITED(st))
			rc = WEXITSTATUS(st);
		else if (WIFSIGNALED(st))
			rc = ERR_TERMIMATED;
		else
			rc = ERR_WAITPID;
	} else if (rc == 1 && WIFEXITED(st) &&
	    (WEXITSTATUS(st) == ERR_FILESELECT ||
	     WEXITSTATUS(st) == ERR_FILE_IO)) {
		/* We only lost a server that couldn't use its destination. */
		rc = WEXITSTATUS(st);
	}

	exit(rc);
usage:
	fprintf(stderr, "usage: %s"
	    " [-acDgIJlnOoprtVvx] [-e program] [--address=sourceaddr]\n"
	    "\t[--contimeout=seconds] [--compare-dest=dir] [--copy-dest=dir]\n"
	    "\t[--link-dest=dir] [--del] [--exclude]\n"
	    "\t[--exclude-from=file] [--include] [--include-from=file]\n"
	    "\t[--no-motd] [--numeric-ids] [--port=portnumber] [--progress]\n"
	    "\t[--rsync-path=program] [--size-only] [--timeout=seconds]\n"
	    "\tsource ... directory\n",
	    getprogname());
	exit(ERR_SYNTAX);
}
