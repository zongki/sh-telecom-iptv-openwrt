#ifndef IPTV_HTTPC_H
#define IPTV_HTTPC_H

#include <stddef.h>
#include <sys/types.h>

/* Minimal HTTP/1.1 client for the IPTV auth chain: GET/POST with
 * urlencoded form bodies, a small cookie jar, redirect following
 * (<=5; 301/302/303 switch POST to GET like urllib does) and
 * Content-Length / chunked / EOF-delimited bodies.
 *
 * Plain http:// only - every Shanghai Telecom auth endpoint is plain
 * HTTP, so the package keeps its zero-TLS dependency policy.
 *
 * A "form" argument is a NULL-terminated array of alternating
 * name/value strings: { "Action", "Login", ..., NULL }.
 */

/* URL buffers: real samples are the ~600-byte playURL and ~900-byte
 * per-segment URLs (AuthInfo-bearing); 2048 keeps >2x headroom. */
#define HTTPC_URL_MAX  2048
/* Absolute in-memory cap for non-streaming response bodies.  Real
 * sample: a 642-segment TVOD media playlist with per-segment AuthInfo
 * URLs measures 593226 bytes; 2MB gives ~3x headroom.  Media segments
 * are streamed (httpc_get_stream) and never hit this. */
#define HTTPC_BODY_MAX (2 * 1024 * 1024)

struct httpc_resp {
	char url[HTTPC_URL_MAX];   /* final URL after redirects */
	int  status;
	char *body;                /* malloc'd, NUL-terminated */
	size_t len;
};

struct httpc;                  /* cookie jar holder, opaque */

struct httpc *httpc_new(void);
void httpc_free(struct httpc *c);

/* referer/origin/cookie may be NULL.  Returns 0 on success (resp
 * filled; free with httpc_resp_free), -1 on transport error. */
int httpc_request(struct httpc *c, const char *url, const char *method,
		  const char *const *form, const char *referer,
		  const char *origin, const char *cookie,
		  int timeout_ms, struct httpc_resp *resp);

/* Same, with an explicit User-Agent (NULL = the built-in set-top
 * webkit string).  The 0.5 VOD playlist/segment fetches must carry the
 * STB RTSP UA the carrier servers expect. */
int httpc_request_ua(struct httpc *c, const char *url, const char *method,
		     const char *const *form, const char *referer,
		     const char *origin, const char *cookie,
		     const char *ua, int timeout_ms,
		     struct httpc_resp *resp);

/* Streaming GET (0.5 VOD): follows redirects like httpc_request, then
 * hands back an open response-body reader instead of buffering it -
 * TVOD segments are multi-megabyte TS files, way past HTTPC_BODY_MAX.
 * NULL on transport error / too many redirects; *status_out gives the
 * final status (check it: non-200 still returns a reader). */
struct httpc_stream *httpc_get_stream(struct httpc *c, const char *url,
				      const char *ua, int timeout_ms,
				      int *status_out);
/* recv()-like: >0 bytes read, 0 at body end, -1 on error. */
ssize_t httpc_stream_read(struct httpc_stream *s, void *buf, size_t n);
/* Content-Length bytes not yet read (0 for chunked/EOF bodies). */
size_t httpc_stream_remaining(const struct httpc_stream *s);
void httpc_stream_close(struct httpc_stream *s);
/* 1.1: unblock a concurrent httpc_stream_read from another thread
 * (shutdown only; httpc_stream_close still owns the close and free). */
void httpc_stream_abort(struct httpc_stream *s);

void httpc_resp_free(struct httpc_resp *resp);

/* Percent-encode for query strings / form bodies ('@' -> %40 ...). */
void httpc_url_encode(const char *in, char *out, size_t cap);

/* Join a possibly relative reference onto a base URL. */
void httpc_url_join(const char *base, const char *ref, char *out, size_t cap);

#endif
