#ifndef IPTV_VOD_H
#define IPTV_VOD_H

#include <stddef.h>
#include <stdint.h>

/* 0.5: per-program TVOD relay (python app/iptv/vod.py).  epg7
 * getTvodPlayUrl hands out an authenticated HTTP HLS playURL (a :8006
 * URL that 302s to the playlist on :8112; ~10s TS segments on :8114).
 * We fetch the segments in order and concatenate them into one
 * video/mp2t stream, paced to the EXTINF durations so player buffers
 * don't grow without bound; failed segments are skipped, not fatal. */

#define VOD_SEG_MAX  2048
#define VOD_SID_MAX  64

/* parsed media playlist: absolute URLs in one pool + durations */
struct vod_seglist {
	char *pool;                       /* NUL-separated absolute URLs */
	size_t pool_len, pool_cap;
	uint32_t off[VOD_SEG_MAX];
	uint32_t dur_ms[VOD_SEG_MAX];
	int n;
};

struct vod_stream;

/* test/debug entry: parse one playlist body into sl; a master
 * playlist names its variant in child (caller recurses), a media
 * playlist appends segments.  Returns segments added, -1 on overflow. */
int vod_parse_playlist(const char *body, const char *base_url,
		       struct vod_seglist *sl, char *child,
		       size_t child_cap);

/* Open a stream on a playURL.  sid optionally binds a speed session
 * (see vod_sess_set); NULL/"" = fixed 1.0x.  NULL + err on failure. */
struct vod_stream *vod_open(const char *play_url, const char *sid,
			    char *err, size_t errcap);

/* Next chunk of the concatenated TS stream, or -1 when it has ended
 * (caller then calls vod_close). */
int vod_read(struct vod_stream *v, void *out, size_t max);
/* 1.1: unblock a concurrent vod_read (the xcode pump) without freeing;
 * vod_close still owns the release. */
void vod_halt(struct vod_stream *v);
void vod_close(struct vod_stream *v);

/* Speed sessions: /api/vod_speed stores per-sid playback speed; the
 * segment pacing follows it so the frontend can match playbackRate
 * without reconnecting.  Clamped to [0.25, 4.0], two decimals. */
int    vod_sid_ok(const char *sid);
int    vod_sess_set(const char *sid, double speed);  /* 0 on success */
double vod_sess_get(const char *sid);                /* 1.0 if absent */

#endif
