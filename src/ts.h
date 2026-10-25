#ifndef IPTV_TS_H
#define IPTV_TS_H

#include <stdint.h>
#include <stddef.h>

/* 0.4: RTSP timeshift client, ported from the python reference
 * (app/iptv/timeshift.py).
 *
 * The carrier server is FonsView: DESCRIBE follows a 302 chain (one
 * connection per hop) until a 200 whose Content-Base points at the
 * 55554 streaming endpoint with an AuthInfo query and a
 * DURATION=<UTC>-<UTC> rolling replay window.  SETUP negotiates
 * MP2T/TCP;interleaved=0-1, PLAY accepts Range: clock=<UTC>-, and the
 * $-framed interleaved payload is raw MPEG-TS. */

struct ts_stream;

/* Follow the 302 DESCRIBE chain; fills content_base (the streaming
 * URL).  0 on success, -1 on error (err gets a reason). */
int ts_resolve(const char *base_url, char *content_base, size_t cb_cap,
	       char *err, size_t errcap);

/* DURATION=<YYYYMMDDTHHMMSSZ>-<...> from the Content-Base -> epoch
 * seconds; 0 if absent. */
int ts_parse_window(const char *content_base, int64_t *start, int64_t *end);

/* SETUP + PLAY on the content-base.  start_epoch > 0: absolute replay
 * point; otherwise back_seconds > 0 replays N seconds back; both zero
 * starts at the live edge. */
struct ts_stream *ts_open(const char *content_base, int64_t start_epoch,
			  int back_seconds, char *err, size_t errcap);

/* Blocking read of raw TS bytes; >0 copied, -1 stream ended. */
int ts_read(struct ts_stream *s, void *buf, size_t max);

/* 1.1: unblock a concurrent ts_read without freeing; ts_close still
 * owns the close and the free. */
void ts_halt(struct ts_stream *s);

void ts_close(struct ts_stream *s);

#endif
