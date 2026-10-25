/* See vod.h.  The pacing contract mirrors vod.py exactly: segments
 * are issued at real-time rhythm by EXTINF duration (divided by the
 * current speed), sleeps are cut into <=250ms slices so a speed change
 * re-paces from "now" within a quarter second, and a slow download
 * naturally catches up because next_t only moves forward by durations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

#include "vod.h"
#include "httpc.h"
#include "iptv.h"
#include "log.h"
#include "util.h"

/* the STB user agent the carrier HLS servers expect (sic: "HMTL") */
#define VOD_SEG_UA      "HMTL RTSP 1.0; CTC/2.0"
#define VOD_DUR_DEF_MS  10000            /* EXTINF missing/garbage -> 10s */
#define VOD_SESS_MAX    64
#define VOD_SESS_TTL_MS (6 * 3600 * 1000)
#define VOD_DEPTH_MAX   2                /* master -> variant nesting */
/* Send-ahead credit on the pace clock.  Measured on wan.85 (r17): the
 * carrier delivers each ~11MB segment as a ~1.4s burst then silence,
 * so a stream paced strictly on the clock leaves the client buffer at
 * zero on every segment boundary and any jitter freezes the player.
 * With a 20s credit the first 2-3 segments stream back-to-back, the
 * client builds a ~20s buffer, and the long-run rate stays 1x. */
#define VOD_LEAD_MS     20000
/* Smooth-delivery meter.  The same wan.85 measurement showed the
 * browser (mpegts.js, stash off, MSE-live) cannot take those bursts
 * either: SourceBuffer appends fall behind, buffered ranges grow
 * holes and playback freezes - while the NAS reference path, which
 * the carrier delivers as a *smooth* 1x stream, plays the same
 * content fine.  So meter the client-facing bytes with a token
 * bucket: accrue at the session bitrate (x2 for the first 20s to
 * bank a player-side buffer), cap bursts at the bucket size.  The
 * lead above keeps segment GETs early so the meter always has data
 * in flight. */
#define VOD_SM_RATE0    1200000      /* bytes/s until 1st segment ends */
#define VOD_SM_TOKENS0  (6 * 1024 * 1024)
#define VOD_SM_CAP      (8 * 1024 * 1024)
#define VOD_SM_BOOST_MS 20000
#define VOD_SM_REL_MIN  65536
#define VOD_SM_SLEEP_MS 40

/* -------- segment list -------- */
/* (struct vod_seglist lives in vod.h for the host tests) */

static int sl_push(struct vod_seglist *sl, const char *url, double dur)
{
	if (sl->n >= VOD_SEG_MAX)
		return -1;
	size_t need = strlen(url) + 1;
	if (sl->pool_len + need > sl->pool_cap) {
		size_t cap = sl->pool_cap ? sl->pool_cap * 2 : 8192;
		while (cap < sl->pool_len + need)
			cap *= 2;
		char *p = realloc(sl->pool, cap);
		if (!p)
			return -1;
		sl->pool = p;
		sl->pool_cap = cap;
	}
	sl->off[sl->n] = (uint32_t)sl->pool_len;
	memcpy(sl->pool + sl->pool_len, url, need);
	sl->pool_len += need;
	sl->dur_ms[sl->n] = dur > 0.0 ? (uint32_t)(dur * 1000.0 + 0.5)
				       : VOD_DUR_DEF_MS;
	sl->n++;
	return 0;
}

/* next whitespace/CRLF-trimmed line (python: splitlines + strip);
 * returns 0 when the input is exhausted */
static int next_line(const char **p, char *out, size_t cap)
{
	const char *base = *p;
	if (!*base)
		return 0;
	const char *nl = strchr(base, '\n');
	size_t len = nl ? (size_t)(nl - base) : strlen(base);
	*p = nl ? nl + 1 : base + len;
	const char *s = base;
	while (len && (*s == ' ' || *s == '\t')) {
		s++;
		len--;
	}
	while (len && (s[len - 1] == '\r' || s[len - 1] == ' ' ||
		       s[len - 1] == '\t'))
		len--;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, s, len);
	out[len] = '\0';
	return 1;
}

/* Parse one playlist body (python _load_playlist).  Master playlists
 * (any #EXT-X-STREAM-INF) name a variant in child[] for the caller to
 * recurse into; media playlists append segment URLs (absolutized
 * against base_url) + EXTINF durations to sl.  Returns the segment
 * count appended, or -1 on overflow. */
int vod_parse_playlist(const char *body, const char *base_url,
		       struct vod_seglist *sl, char *child, size_t child_cap)
{
	child[0] = '\0';

	int master = 0;
	const char *p = body;
	char line[2048];   /* real segment URLs run ~900B (AuthInfo) */
	while (next_line(&p, line, sizeof(line))) {
		if (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0) {
			master = 1;
			break;
		}
	}

	int added = 0;
	p = body;
	double dur = 0.0;
	while (next_line(&p, line, sizeof(line))) {
		if (!line[0])
			continue;
		if (strncmp(line, "#EXTINF:", 8) == 0) {
			dur = strtod(line + 8, NULL);   /* stops at the comma */
		} else if (line[0] == '#') {
			continue;
		} else if (master) {
			/* first non-comment line = the variant playlist */
			if (!child[0])
				snprintf(child, child_cap, "%s", line);
		} else {
			char abs[HTTPC_URL_MAX];
			httpc_url_join(base_url, line, abs, sizeof(abs));
			if (sl_push(sl, abs, dur) != 0)
				return -1;
			added++;
			dur = 0.0;
		}
	}
	return added;
}

/* -------- playlist fetch (redirects + master->variant nesting) -------- */

static int fail(char *err, size_t errcap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errcap, fmt, ap);
	va_end(ap);
	log_msg(LL_WARN, "vod: %s", err);
	return -1;
}

static int load_playlist(const char *url, int depth,
			 struct vod_seglist *sl, char *err, size_t errcap)
{
	struct httpc *c = httpc_new();
	if (!c)
		return fail(err, errcap, "out of memory");
	struct httpc_resp resp;
	int rc = httpc_request_ua(c, url, "GET", NULL, NULL, NULL, NULL,
				  VOD_SEG_UA, 20000, &resp);
	char child[HTTPC_URL_MAX] = "";
	char final_url[HTTPC_URL_MAX] = "";
	if (rc == 0) {
		snprintf(final_url, sizeof(final_url), "%s", resp.url);
		if (resp.status != 200 || !resp.body) {
			httpc_resp_free(&resp);
			httpc_free(c);
			return fail(err, errcap, "playlist %s: status %d",
				    url, resp.status);
		}
		vod_parse_playlist(resp.body, final_url, sl, child,
				   sizeof(child));
	}
	httpc_resp_free(&resp);
	httpc_free(c);
	if (rc != 0)
		return fail(err, errcap, "playlist %s fetch failed", url);

	if (child[0]) {
		if (depth >= VOD_DEPTH_MAX)
			return fail(err, errcap, "playlist nesting > %d",
				    VOD_DEPTH_MAX);
		char abs[HTTPC_URL_MAX];
		httpc_url_join(final_url, child, abs, sizeof(abs));
		return load_playlist(abs, depth + 1, sl, err, errcap);
	}
	if (sl->n == 0)
		return fail(err, errcap, "playlist has no segments");
	return 0;
}

/* -------- speed sessions (python VOD_SESSIONS + /api/vod_speed) -------- */

struct vod_sess {
	char sid[VOD_SID_MAX + 1];
	double speed;
	int64_t last_ms;
};

static struct {
	pthread_mutex_t lock;
	struct vod_sess tab[VOD_SESS_MAX];
	int n;
} VS = { .lock = PTHREAD_MUTEX_INITIALIZER };

int vod_sid_ok(const char *sid)
{
	if (!sid || !*sid || strlen(sid) > VOD_SID_MAX)
		return 0;
	for (const char *p = sid; *p; p++)
		if (!isalnum((unsigned char)*p))
			return 0;
	return 1;
}

double vod_sess_get(const char *sid)
{
	double sp = 1.0;
	if (!sid || !*sid)
		return sp;
	pthread_mutex_lock(&VS.lock);
	for (int i = 0; i < VS.n; i++) {
		if (strcmp(VS.tab[i].sid, sid) == 0) {
			VS.tab[i].last_ms = util_now_ms();
			sp = VS.tab[i].speed;
			break;
		}
	}
	pthread_mutex_unlock(&VS.lock);
	return sp;
}

int vod_sess_set(const char *sid, double speed)
{
	if (!vod_sid_ok(sid))
		return -1;
	if (speed < 0.25)
		speed = 0.25;
	if (speed > 4.0)
		speed = 4.0;
	speed = (double)((int)(speed * 100.0 + 0.5)) / 100.0;

	pthread_mutex_lock(&VS.lock);
	struct vod_sess *e = NULL;
	for (int i = 0; i < VS.n; i++)
		if (strcmp(VS.tab[i].sid, sid) == 0) {
			e = &VS.tab[i];
			break;
		}
	if (!e) {
		if (VS.n >= VOD_SESS_MAX) {
			/* evict the least recently used */
			int old = 0;
			for (int i = 1; i < VS.n; i++)
				if (VS.tab[i].last_ms < VS.tab[old].last_ms)
					old = i;
			VS.tab[old] = VS.tab[--VS.n];
			e = &VS.tab[VS.n];
		} else {
			e = &VS.tab[VS.n++];
		}
		snprintf(e->sid, sizeof(e->sid), "%s", sid);
	}
	e->speed = speed;
	e->last_ms = util_now_ms();
	pthread_mutex_unlock(&VS.lock);
	return 0;
}

/* -------- stream -------- */

struct vod_stream {
	struct vod_seglist sl;
	int cur;
	struct httpc *hc;               /* per-stream client (cookie jar) */
	struct httpc_stream *seg;       /* currently open segment body */
	int64_t next_t_ms;              /* monotonic pace clock */
	double last_sp;
	char sid[VOD_SID_MAX + 1];      /* speed session binding, "" = none */
	/* 1.1 xcode pump teardown: mu serializes seg pointer mutations
	 * against vod_halt (shutdown vs close fd-reuse race), halt makes
	 * reads return -1 promptly */
	pthread_mutex_t mu;
	volatile int halt;
	/* smooth-delivery meter (see VOD_SM_* above) */
	double sm_rate;
	int64_t sm_tokens;
	int64_t sm_t0_ms;
	int64_t sm_last_ms;
	size_t seg_clen, seg_read;
};
/* current speed from the bound session, floored like vod.py (0.1x) */
static double speed_of(const struct vod_stream *v)
{
	double sp = v->sid[0] ? vod_sess_get(v->sid) : 1.0;
	return sp < 0.1 ? 0.1 : sp;
}

struct vod_stream *vod_open(const char *play_url, const char *sid,
			    char *err, size_t errcap)
{
	struct vod_stream *v = calloc(1, sizeof(*v));
	if (!v) {
		fail(err, errcap, "out of memory");
		return NULL;
	}
	if (sid && vod_sid_ok(sid))
		snprintf(v->sid, sizeof(v->sid), "%s", sid);
	if (load_playlist(play_url, 0, &v->sl, err, errcap) != 0) {
		vod_close(v);
		return NULL;
	}
	v->hc = httpc_new();
	if (!v->hc) {
		vod_close(v);
		fail(err, errcap, "out of memory");
		return NULL;
	}
	v->last_sp = speed_of(v);
	v->next_t_ms = util_now_ms();
	v->sm_rate = VOD_SM_RATE0;
	v->sm_tokens = VOD_SM_TOKENS0;
	v->sm_t0_ms = v->sm_last_ms = util_now_ms();
	pthread_mutex_init(&v->mu, NULL);
	return v;
}

/* accrue meter tokens up to now (rate x speed, x2 during the opening
 * buffer-fill window, capped at VOD_SM_CAP) */
static void sm_refill(struct vod_stream *v)
{
	int64_t now = util_now_ms();
	if (now <= v->sm_last_ms)
		return;
	double mult = speed_of(v);
	if (now - v->sm_t0_ms < VOD_SM_BOOST_MS)
		mult *= 2.0;
	double add = (double)(now - v->sm_last_ms) * v->sm_rate * mult / 1000.0;
	v->sm_tokens = (int64_t)((double)v->sm_tokens + add);
	if (v->sm_tokens > VOD_SM_CAP)
		v->sm_tokens = VOD_SM_CAP;
	v->sm_last_ms = now;
}

/* sleep until the stream is due next_t (with the VOD_LEAD_MS send-ahead
 * credit) in <=250ms slices; a speed change re-paces from the current
 * moment (python: sp != last_sp -> next_t = now) */
static void pace_wait(struct vod_stream *v)
{
	for (;;) {
		if (v->halt)
			return;
		int64_t wait = v->next_t_ms - VOD_LEAD_MS - util_now_ms();
		if (wait <= 50)
			return;
		util_sleep_ms(wait > 250 ? 250 : wait);
		if (!v->sid[0])
			continue;
		double sp = vod_sess_get(v->sid);
		if (sp != v->last_sp) {
			v->last_sp = sp;
			v->next_t_ms = util_now_ms();
		}
	}
}

/* segment finished (or skipped): advance the pace clock by its
 * duration at the current speed */
static void advance(struct vod_stream *v)
{
	v->last_sp = speed_of(v);
	v->next_t_ms += (int64_t)(v->sl.dur_ms[v->cur] / v->last_sp);
	v->cur++;
}

/* r6: the carrier's landing nodes occasionally emit a full 188-byte
 * packet of 0xff garbage (measured ~1/minute on the 76-CHC movie;
 * the NAS reference path lands on nodes that never do).  mpegts.js
 * breaks its demuxer on any non-0x47 sync byte mid-stream, freezing
 * playback minutes in.  Rewrite such packets as proper null packets
 * (PID 0x1fff): the 188 grid is preserved and real PID CC counters
 * are untouched.  Reads are 188-aligned per segment (see vod_read),
 * so the scan never spans a buffer boundary. */
static void vod_scrub(unsigned char *b, size_t n)
{
	static const unsigned char null_pkt[188] = { 0x47, 0x1f, 0xff, 0x10 };
	size_t fixed = 0;
	for (size_t i = 0; i + 188 <= n; i += 188) {
		if (b[i] != 0x47) {
			memcpy(b + i, null_pkt, 4);
			memset(b + i + 4, 0xff, 184);
			fixed++;
		}
	}
	if (fixed)
		log_msg(LL_WARN, "vod: scrubbed %zu broken packet(s) from stream", fixed);
}

int vod_read(struct vod_stream *v, void *out, size_t max)
{
	for (;;) {
		if (v->halt)
			return -1;             /* vod_halt: stop feeding */
		if (v->seg) {
			ssize_t n;
			sm_refill(v);
			while (v->sm_tokens < VOD_SM_REL_MIN && !v->halt) {
				util_sleep_ms(VOD_SM_SLEEP_MS);
				sm_refill(v);
			}
			size_t want = max;
			if (want > (size_t)v->sm_tokens)
				want = (size_t)v->sm_tokens;
			if (v->seg_clen && v->seg_read + want > v->seg_clen)
				want = v->seg_clen - v->seg_read;
			/* keep reads on the 188-byte packet grid relative to
			 * the segment start so vod_scrub never spans a chunk;
			 * a <188 tail (malformed segment length) passes through
			 * unscanned rather than being dropped */
			size_t take = want - want % 188;
			int scrub = 1;
			if (!take) {
				take = want;
				scrub = 0;
			}
			n = httpc_stream_read(v->seg, out, take);
			if (n > 0) {
				if (scrub)
					vod_scrub((unsigned char *)out, (size_t)n);
				v->sm_tokens -= n;
				v->seg_read += (size_t)n;
				return (int)n;
			}
			if (n < 0 && !v->halt)
				/* transport died mid-body: the rest of this
				 * segment is lost and playback jumps forward
				 * - say so instead of looking like a clean
				 * segment end */
				log_msg(LL_WARN,
					"vod: segment %d/%d aborted mid-body, rest skipped",
					v->cur + 1, v->sl.n);
			if (n == 0 && v->seg_clen > 0 && v->seg_read == v->seg_clen &&
			    v->sl.dur_ms[v->cur] > 0) {
				/* clean full segment: ratchet the meter rate up
				 * if this VBR segment ran hotter than the
				 * current estimate.  Never ratchet DOWN: a
				 * rate below the content average erodes the
				 * client's buffer lead minute by minute, and
				 * an underrun at the send edge desyncs the
				 * browser demuxer (measured r3 freeze). */
				double seg_rate = (double)v->seg_clen /
						  (v->sl.dur_ms[v->cur] / 1000.0);
				if (seg_rate > v->sm_rate)
					v->sm_rate = seg_rate;
			}
			pthread_mutex_lock(&v->mu);
			httpc_stream_close(v->seg);
			v->seg = NULL;
			pthread_mutex_unlock(&v->mu);
			advance(v);            /* finished: schedule the next */
			continue;
		}
		if (v->cur >= v->sl.n)
			return -1;             /* all segments consumed */
		pace_wait(v);
		if (v->halt)
			return -1;
		int status = 0;
		struct httpc_stream *ns = httpc_get_stream(v->hc,
				v->sl.pool + v->sl.off[v->cur],
				VOD_SEG_UA, 30000, &status);
		pthread_mutex_lock(&v->mu);
		v->seg = ns;
		if (ns && v->halt)
			httpc_stream_abort(ns); /* halt raced the open */
		pthread_mutex_unlock(&v->mu);
		if (!ns || status != 200) {
			log_msg(LL_WARN, "vod: segment %d/%d failed (status %d), skipped",
				v->cur + 1, v->sl.n, ns ? status : 0);
			pthread_mutex_lock(&v->mu);
			if (v->seg) {
				httpc_stream_close(v->seg);
				v->seg = NULL;
			}
			pthread_mutex_unlock(&v->mu);
			advance(v);            /* skip: keep the overall pace */
			continue;
		}
		v->seg_clen = httpc_stream_remaining(ns);
		v->seg_read = 0;
	}
}

/* 1.1: unblock a concurrent vod_read (the xcode pump) without freeing;
 * vod_close still owns the release */
void vod_halt(struct vod_stream *v)
{
	if (!v)
		return;
	v->halt = 1;
	pthread_mutex_lock(&v->mu);
	if (v->seg)
		httpc_stream_abort(v->seg);
	pthread_mutex_unlock(&v->mu);
}

void vod_close(struct vod_stream *v)
{
	if (!v)
		return;
	if (v->seg)
		httpc_stream_close(v->seg);
	httpc_free(v->hc);
	free(v->sl.pool);
	pthread_mutex_destroy(&v->mu);
	free(v);
}
