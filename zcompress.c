/*
 * Real rsync-protocol payload compression for --compress (-z): a
 * session-persistent raw-deflate stream on the send side and a
 * matching raw-inflate stream on the receive side, flushed to a byte
 * boundary (Z_SYNC_FLUSH) after every chunk so each wire unit is
 * independently decodable -- this lets the existing chunked
 * literal-data framing (length-prefixed pieces; see sender.c's
 * BLKSTAT_DATA case and downloader.c's rawtok>0 case) carry compressed
 * bytes with no other protocol change. Match tokens (references to
 * unchanged blocks) are never compressed, matching real rsync -- token
 * indices are already compact integers.
 *
 * This is NOT byte-compatible with GNU rsync's own compression format
 * (which has its own, more involved history across protocol
 * versions). openrsync/smallclue is always on both ends of any
 * --compress transfer smallclue itself initiates -- fargs_cmdline()
 * forwards --compress to the peer invocation (a locally forked/exec'd
 * child, an ssh-invoked remote, or a daemon connection), so only
 * interoperating with itself is required.
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "extern.h"

/*
 * Callers never ask us to (de)compress more than one MAX_CHUNK-sized
 * piece at a time (sender.c caps its literal-data segments at
 * MAX_CHUNK before handing them to us), so a decompressed chunk can
 * never exceed that -- size the receive buffer with generous headroom
 * over it rather than growing it dynamically.
 */
#define ZRECV_BUFSZ	(MAX_CHUNK * 4)

static z_stream *
zsend_get(struct sess *sess)
{
	z_stream	*zs;

	if (sess->zsend != NULL)
		return sess->zsend;

	if ((zs = calloc(1, sizeof(*zs))) == NULL)
		return NULL;
	if (deflateInit2(zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
	    -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		free(zs);
		return NULL;
	}
	sess->zsend = zs;
	return zs;
}

static z_stream *
zrecv_get(struct sess *sess)
{
	z_stream	*zs;

	if (sess->zrecv != NULL)
		return sess->zrecv;

	if ((zs = calloc(1, sizeof(*zs))) == NULL)
		return NULL;
	if (inflateInit2(zs, -15) != Z_OK) {
		free(zs);
		return NULL;
	}
	sess->zrecv = zs;
	return zs;
}

/*
 * Compress "len" bytes of literal file data for the wire. On success,
 * *outbuf is a malloc'd buffer of *outlen compressed bytes -- the
 * caller owns it and must free() it.
 */
int
sess_compress_send(struct sess *sess, const void *data, size_t len,
    void **outbuf, size_t *outlen)
{
	z_stream	*zs;
	unsigned char	*out;
	size_t		 cap, have = 0;

	if ((zs = zsend_get(sess)) == NULL)
		return 0;

	/* True worst-case bound for one Z_SYNC_FLUSH-terminated run. */
	cap = deflateBound(zs, (uLong)len) + 32;
	if ((out = malloc(cap)) == NULL)
		return 0;

	zs->next_in = (unsigned char *)data;
	zs->avail_in = (uInt)len;

	do {
		zs->next_out = out + have;
		zs->avail_out = (uInt)(cap - have);
		if (deflate(zs, Z_SYNC_FLUSH) == Z_STREAM_ERROR) {
			free(out);
			return 0;
		}
		have = cap - zs->avail_out;
	} while (zs->avail_out == 0 && have < cap);

	*outbuf = out;
	*outlen = have;
	return 1;
}

/*
 * Decompress "clen" wire bytes back into literal file data. On
 * success, *outbuf is a malloc'd buffer of *outlen decompressed bytes
 * -- the caller owns it and must free() it.
 */
int
sess_compress_recv(struct sess *sess, const void *cdata, size_t clen,
    void **outbuf, size_t *outlen)
{
	z_stream	*zs;
	unsigned char	*out;
	size_t		 have = 0;
	int		 rc;

	if ((zs = zrecv_get(sess)) == NULL)
		return 0;
	if ((out = malloc(ZRECV_BUFSZ)) == NULL)
		return 0;

	zs->next_in = (unsigned char *)cdata;
	zs->avail_in = (uInt)clen;

	do {
		if (have >= ZRECV_BUFSZ) {
			/* Would only happen if a peer sent us a chunk
			 * bigger than our own sender ever produces. */
			free(out);
			return 0;
		}
		zs->next_out = out + have;
		zs->avail_out = (uInt)(ZRECV_BUFSZ - have);
		rc = inflate(zs, Z_SYNC_FLUSH);
		if (rc != Z_OK && rc != Z_BUF_ERROR) {
			free(out);
			return 0;
		}
		have = ZRECV_BUFSZ - zs->avail_out;
	} while (zs->avail_in > 0);

	*outbuf = out;
	*outlen = have;
	return 1;
}

void
sess_compress_free(struct sess *sess)
{
	if (sess->zsend != NULL) {
		deflateEnd((z_stream *)sess->zsend);
		free(sess->zsend);
		sess->zsend = NULL;
	}
	if (sess->zrecv != NULL) {
		inflateEnd((z_stream *)sess->zrecv);
		free(sess->zrecv);
		sess->zrecv = NULL;
	}
}
