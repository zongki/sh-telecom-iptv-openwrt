/* HTTP layer on libmicrohttpd (API subset stable 0.9.59..1.x).
 *
 * udpxy-compatible URL space so existing players keep working:
 *   /udp/<group>:<port>   /udp/<group>/<port>   relay live multicast
 *   /rtp/<group>:<port>   (same handler)
 *   /status                                   plain-text status
 *   /playlist.m3u                            from /etc/iptv/channels.m3u
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <microhttpd.h>

#include "iptv.h"
#include "log.h"
#include "mcast.h"
#include "fcc.h"
#include "httpd.h"
#include "auth.h"
#include "epg.h"
#include "httpc.h"
#include "ts.h"
#include "vod.h"
#include "xcode.h"
#include "util.h"

#define PLAYLIST_PATH "/etc/iptv/channels.m3u"

/* MHD >= 0.9.71 (OpenWrt 21.02+) returns enum MHD_Result from handler
 * callbacks; 19.07's 0.9.59 uses int.  Same call sites either way. */
#if defined(MHD_VERSION) && MHD_VERSION >= 0x00097100
#define MHD_HANDLER_RET enum MHD_Result
#else
#define MHD_HANDLER_RET int
#endif

static struct iptv_cfg *CFG;
static int64_t started_ms;

/* -------- streaming -------- */

static ssize_t relay_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)pos;
	struct mcast_client *c = cls;
	int n = mcast_relay_read(c, buf, max);
	if (n < 0)
		return -1;
	/* An empty read is not end-of-stream: the FCC push runs ahead of
	 * the local multicast view, and right after the FMT=4 merge the
	 * relay waits (seconds) for multicast to catch up to the last
	 * pushed seq.  Cutting here froze playback mid-merge; the client
	 * rides over the wait on its own buffer. */
	return n == 0 ? 0 : (ssize_t)n;
}

static void relay_free(void *cls)
{
	mcast_relay_close(cls);
}

/* xcode source adapters (plain shapes; the MHD-shaped readers stay
 * above) */
static ssize_t relay_src_read(void *cls, char *buf, size_t max)
{
	return mcast_relay_read(cls, buf, max);
}

static void relay_src_halt(void *cls)
{
	mcast_relay_halt(cls);
}

static MHD_HANDLER_RET handle_relay(struct MHD_Connection *conn, const char *url)
{
	const char *rest = url;
	if (!strncmp(rest, "/udp/", 5))
		rest += 5;
	else if (!strncmp(rest, "/rtp/", 5))
		rest += 5;
	else
		return MHD_NO;

	char ipbuf[64];
	int i = 0;
	while (rest[i] && rest[i] != ':' && rest[i] != '/' && i < (int)sizeof(ipbuf) - 1) {
		ipbuf[i] = rest[i];
		i++;
	}
	ipbuf[i] = '\0';
	if (rest[i] != ':' && rest[i] != '/')
		return MHD_NO;
	int port = atoi(rest + i + 1);
	in_addr_t group = inet_addr(ipbuf);
	if (group == (in_addr_t)-1 || port < 1 || port > 65535 ||
	    !IN_MULTICAST(ntohl(group))) {
		static const char bad[] = "bad request\n";
		struct MHD_Response *r = MHD_create_response_from_buffer(
			sizeof(bad) - 1, (void *)bad, MHD_RESPMEM_PERSISTENT);
		int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, r);
		MHD_destroy_response(r);
		return ret;
	}

	if (mcast_client_count() >= CFG->max_clients) {
		static const char busy[] = "too many clients\n";
		struct MHD_Response *r = MHD_create_response_from_buffer(
			sizeof(busy) - 1, (void *)busy, MHD_RESPMEM_PERSISTENT);
		int ret = MHD_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
		MHD_destroy_response(r);
		return ret;
	}

	/* the channel's operator-designated FCC server (r16); ?nofcc=1
	 * (r9) skips the accelerated push - the player's degrade path
	 * after a decode error rejoins on plain multicast, whose stream
	 * is clean where the carrier's push occasionally drops frames */
	int fcc_en = !MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
						  "nofcc");
	char fccsrv[48];
	const char *fs = fcc_en && fcc_enabled() &&
			 epg_fcc_for_group(group, port, fccsrv, sizeof(fccsrv))
			 ? fccsrv : NULL;
	struct mcast_client *c = mcast_relay_open(group, port, fs, fcc_en);
	if (!c) {
		static const char err[] = "relay unavailable (join failed / disabled)\n";
		struct MHD_Response *r = MHD_create_response_from_buffer(
			sizeof(err) - 1, (void *)err, MHD_RESPMEM_PERSISTENT);
		int ret = MHD_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
		MHD_destroy_response(r);
		return ret;
	}

	/* 1.1: AC-3 (4K) channels get their audio transcoded to AAC, or
	 * browser MSE plays video-only with a dead volume control; without
	 * ffmpeg on the device the raw relay is served as before */
	struct xcode_sess *xc = NULL;
	if (CFG->xcode && epg_ac3_for_group(group, port)) {
		struct xcode_src xs = {
			.read = relay_src_read,
			.halt = relay_src_halt,
			.free = relay_free,
			.src = c,
		};
		xc = xcode_open(&xs);
	}

	struct MHD_Response *r = MHD_create_response_from_callback(
		MHD_SIZE_UNKNOWN, 32 * 1024,
		xc ? xcode_read : relay_reader,
		xc ? (void *)xc : (void *)c,
		xc ? xcode_close : relay_free);
	if (!r) {
		if (xc)
			xcode_close(xc);
		else
			mcast_relay_close(c);
		return MHD_NO;
	}
	MHD_add_response_header(r, "Content-Type", "video/mp2t");
	MHD_add_response_header(r, "Cache-Control", "no-store");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	log_msg(LL_INFO, "relay: client start %s:%d%s", ipbuf, port,
		xc ? " (ac3->aac)" : "");
	return ret;
}

/* -------- status / playlist -------- */

static int send_text(struct MHD_Connection *conn, const char *text,
		     int status_code, const char *ctype)
{
	char *buf = strdup(text);
	if (!buf)
		return MHD_NO;
	struct MHD_Response *r = MHD_create_response_from_buffer(
		strlen(buf), buf, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(r, "Content-Type", ctype);
	int ret = MHD_queue_response(conn, status_code, r);
	MHD_destroy_response(r);
	return ret;
}

/* read a whole file into a malloc'd NUL-terminated buffer; NULL when
 * absent, unreadable or larger than cap */
static char *read_file(const char *path, size_t cap, size_t *outlen)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0 || (size_t)sz > cap) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[sz] = '\0';
	*outlen = (size_t)sz;
	return buf;
}

/* -------- epg API (0.3) -------- */

static MHD_HANDLER_RET handle_playlist_static(struct MHD_Connection *conn);
static MHD_HANDLER_RET handle_index(struct MHD_Connection *conn);

static MHD_HANDLER_RET handle_api_channels(struct MHD_Connection *conn)
{
	struct sbuf sb;
	if (sb_init(&sb, 8192) != 0)
		return MHD_NO;
	/* ~1KB per channel: size the snapshot by the live count, not the
	 * table maximum (heap - MHD worker threads get musl's 128KB stack) */
	int n = epg_channel_count();
	struct epg_channel *chans =
		malloc(sizeof(struct epg_channel) * (size_t)(n > 0 ? n : 1));
	if (!chans)
		return MHD_NO;
	n = epg_channels_copy(chans, n);
	sb_printf(&sb, "{\"count\":%d,\"channels\":[", n);
	for (int i = 0; i < n; i++) {
		struct epg_channel *c = &chans[i];
		if (i)
			sb_printf(&sb, ",");
		sb_printf(&sb, "{\"no\":%d,\"name\":", c->no);
		sb_json_str(&sb, c->name);
		sb_printf(&sb, ",\"pid\":");
		sb_json_str(&sb, c->pid);
		sb_printf(&sb, ",\"chid\":");
		sb_json_str(&sb, c->chid);
		sb_printf(&sb, ",\"igmp\":");
		sb_json_str(&sb, c->igmp);
		sb_printf(&sb, ",\"ts\":%d,\"hd\":%d,\"ac3\":%d,\"cats\":",
			  c->ts, c->hd, c->ac3);
		sb_json_str(&sb, c->cats);
		sb_printf(&sb, "}");
	}
	sb_printf(&sb, "]}");
	int ret = send_text(conn, sb.b, MHD_HTTP_OK, "application/json");
	sb_free(&sb);
	free(chans);
	return ret;
}

/* url: /api/programs/<pid>; value after the last '/' */
static MHD_HANDLER_RET handle_api_programs(struct MHD_Connection *conn, const char *url)
{
	const char *pid = strrchr(url, '/') + 1;
	const struct epg_prog *progs;
	int n = epg_programs(pid, &progs);
	if (n < 0)
		return send_text(conn, "{\"error\":\"no such channel\"}",
				 MHD_HTTP_NOT_FOUND, "application/json");
	struct sbuf sb;
	if (sb_init(&sb, 8192) != 0)
		return MHD_NO;
	sb_printf(&sb, "{\"pid\":");
	sb_json_str(&sb, pid);
	sb_printf(&sb, ",\"count\":%d,\"programs\":[", n);
	for (int i = 0; i < n; i++) {
		if (i)
			sb_printf(&sb, ",");
		sb_printf(&sb, "{\"id\":");
		sb_json_str(&sb, progs[i].id);
		sb_printf(&sb, ",\"name\":");
		sb_json_str(&sb, progs[i].name);
		sb_printf(&sb, ",\"start\":%lld,\"end\":%lld}",
			  (long long)progs[i].start_ms, (long long)progs[i].end_ms);
	}
	sb_printf(&sb, "]}");
	int ret = send_text(conn, sb.b, MHD_HTTP_OK, "application/json");
	sb_free(&sb);
	return ret;
}

static MHD_HANDLER_RET handle_api_now(struct MHD_Connection *conn, const char *url)
{
	const char *pid = strrchr(url, '/') + 1;
	char *data = epg_now_next(pid);
	if (!data)
		return send_text(conn, "{\"error\":\"no data\"}",
				 MHD_HTTP_NOT_FOUND, "application/json");
	struct sbuf sb;
	if (sb_init(&sb, 256) != 0) {
		free(data);
		return MHD_NO;
	}
	sb_printf(&sb, "{\"pid\":");
	sb_json_str(&sb, pid);
	sb_printf(&sb, ",\"data\":%s}", data);
	free(data);
	int ret = send_text(conn, sb.b, MHD_HTTP_OK, "application/json");
	sb_free(&sb);
	return ret;
}

static MHD_HANDLER_RET handle_api_search(struct MHD_Connection *conn)
{
	const char *q = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "q");
	if (!q || !*q)
		return send_text(conn, "{\"error\":\"missing q\"}",
				 MHD_HTTP_BAD_REQUEST, "application/json");
	int building;
	int64_t age;
	epg_search_state(&building, &age);
	char *items = epg_search(q);
	if (!items) {
		char msg[96];
		snprintf(msg, sizeof(msg),
			 "{\"building\":%d,\"error\":\"index %s\"}", building,
			 building ? "building" : "not ready");
		return send_text(conn, msg, MHD_HTTP_SERVICE_UNAVAILABLE,
				 "application/json");
	}
	struct sbuf sb;
	if (sb_init(&sb, 4096) != 0) {
		free(items);
		return MHD_NO;
	}
	sb_printf(&sb, "{\"q\":");
	sb_json_str(&sb, q);
	sb_printf(&sb, ",\"building\":%d,\"age\":%lld,\"items\":%s}",
		  building, (long long)age, items);
	free(items);
	int ret = send_text(conn, sb.b, MHD_HTTP_OK, "application/json");
	sb_free(&sb);
	return ret;
}

/* dynamic playlist from the merged channel table, udpxy-style relay
 * URLs so any player works out of the box; falls back to the static
 * file when the auth chain is off */
static MHD_HANDLER_RET handle_playlist(struct MHD_Connection *conn)
{
	int n = epg_channel_count();
	if (n == 0)
		return handle_playlist_static(conn);

	const char *host = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Host");
	char hostbuf[72];
	if (!host || !*host)
		snprintf(hostbuf, sizeof(hostbuf), "%s:%d", CFG->bind, CFG->port);
	else
		snprintf(hostbuf, sizeof(hostbuf), "%s", host);

	/* ~1KB per channel: size the snapshot by the live count (heap -
	 * MHD worker threads get musl's 128KB stack) */
	struct epg_channel *chans =
		malloc(sizeof(struct epg_channel) * (size_t)(n > 0 ? n : 1));
	if (!chans)
		return MHD_NO;
	n = epg_channels_copy(chans, n);
	struct sbuf sb;
	if (sb_init(&sb, 8192) != 0) {
		free(chans);
		return MHD_NO;
	}
	sb_printf(&sb, "#EXTM3U\n");
	for (int i = 0; i < n; i++) {
		struct epg_channel *c = &chans[i];
		/* igmp://g:p or rtp://g:p -> /udp/g:p through ourselves */
		const char *gp = strstr(c->igmp, "://");
		if (!gp)
			continue;
		gp += 3;
		if (!strchr(gp, ':'))
			continue;
		sb_printf(&sb, "#EXTINF:-1 tvg-id=");
		sb_json_str(&sb, c->pid);
		if (c->cats[0]) {
			sb_printf(&sb, " group-title=");
			sb_json_str(&sb, c->cats);
		}
		sb_printf(&sb, ",%s\nhttp://%s/udp/%s\n", c->name, hostbuf, gp);
	}
	int ret = send_text(conn, sb.b, MHD_HTTP_OK, "audio/x-mpegurl");
	sb_free(&sb);
	free(chans);
	return ret;
}

/* -------- timeshift (0.4) -------- */

static ssize_t ts_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)pos;
	return ts_read(cls, buf, max);
}

static void ts_free(void *cls)
{
	ts_close(cls);
}

/* xcode source adapters */
static ssize_t ts_src_read(void *cls, char *buf, size_t max)
{
	return ts_read(cls, buf, max);
}

static void ts_src_halt(void *cls)
{
	ts_halt(cls);
}

static void ts_src_free(void *cls)
{
	ts_close(cls);
}

/* one resolve+open attempt with window clamping; NULL + err on fail */
static struct ts_stream *ts_attempt(const struct epg_channel *ch,
				    int64_t start_epoch, int back,
				    char *err, size_t errcap)
{
	char cb[1024];
	if (ts_resolve(ch->timeshift_url, cb, sizeof(cb), err, errcap) != 0)
		return NULL;
	if (start_epoch > 0) {
		int64_t w_start, w_end;
		if (ts_parse_window(cb, &w_start, &w_end) && start_epoch < w_start) {
			log_msg(LL_INFO, "ts: clamp start to window %lld",
				(long long)w_start);
			start_epoch = w_start;
		}
	}
	return ts_open(cb, start_epoch, back, err, errcap);
}

/* url: /ts/<pid>.ts?back=N / ?start=T / ?utc=T (python /ts rules) */
static MHD_HANDLER_RET handle_ts(struct MHD_Connection *conn, const char *url)
{
	char pid[40];
	snprintf(pid, sizeof(pid), "%s", strrchr(url, '/') + 1);
	char *dot = strrchr(pid, '.');
	if (dot && strcmp(dot, ".ts") == 0)
		*dot = '\0';

	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0)
		return send_text(conn, "{\"error\":\"no such channel\"}",
				 MHD_HTTP_NOT_FOUND, "application/json");
	if (!ch.ts || !ch.timeshift_url[0])
		return send_text(conn, "{\"error\":\"channel not timeshift-capable\"}",
				 MHD_HTTP_BAD_REQUEST, "application/json");

	const char *a;
	int back = 0;
	int64_t start_epoch = 0;
	if ((a = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "back")))
		back = atoi(a);
	if ((a = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "utc")))
		start_epoch = strtoll(a, NULL, 10);
	if ((a = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "start")))
		start_epoch = strtoll(a, NULL, 10);
	if (start_epoch > 1000000000000LL)   /* ms -> s */
		start_epoch /= 1000;
	if (start_epoch && start_epoch >= (int64_t)time(NULL) - 30) {
		back = 0;                        /* near-now: live edge */
		start_epoch = 0;
	} else if (start_epoch) {
		back = 0;
	}

	char err[1024];
	struct ts_stream *s = NULL;
	for (int attempt = 0; attempt < 2 && !s; attempt++) {
		s = ts_attempt(&ch, start_epoch, back, err, sizeof(err));
		if (!s && attempt == 0 && auth_mgr_enabled()) {
			/* credentials may have expired: force re-auth, reload the
			 * channel table (timeshift_url embeds AuthInfo), retry.
			 * auth_state is ~104KB: heap (musl 128KB worker stacks) */
			struct auth_state *st = malloc(sizeof(*st));
			if (st) {
				if (auth_mgr_force_refresh(st) == 0)
					epg_refresh_channels(st);
				free(st);
			}
			epg_find_channel(pid, &ch);
		}
	}
	if (!s)
		return send_text(conn, err, MHD_HTTP_BAD_GATEWAY, "text/plain");

	/* 1.1: transcode AC-3 audio on the 4K timeshift streams too */
	struct xcode_sess *xc = NULL;
	if (CFG->xcode && ch.ac3) {
		struct xcode_src xs = {
			.read = ts_src_read,
			.halt = ts_src_halt,
			.free = ts_src_free,
			.src = s,
		};
		xc = xcode_open(&xs);
	}

	struct MHD_Response *r = MHD_create_response_from_callback(
		MHD_SIZE_UNKNOWN, 32 * 1024,
		xc ? xcode_read : ts_reader,
		xc ? (void *)xc : (void *)s,
		xc ? xcode_close : ts_free);
	if (!r) {
		if (xc)
			xcode_close(xc);
		else
			ts_close(s);
		return MHD_NO;
	}
	MHD_add_response_header(r, "Content-Type", "video/mp2t");
	MHD_add_response_header(r, "Cache-Control", "no-store");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	log_msg(LL_INFO, "ts: client start %s (back=%d start=%lld)%s",
		pid, back, (long long)start_epoch, xc ? " (ac3->aac)" : "");
	return ret;
}

/* -------- vod (0.5) -------- */

static ssize_t vod_reader(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)pos;
	return vod_read(cls, buf, max);
}

static void vod_free(void *cls)
{
	vod_close(cls);
}

/* xcode source adapters */
static ssize_t vod_src_read(void *cls, char *buf, size_t max)
{
	return vod_read(cls, buf, max);
}

static void vod_src_halt(void *cls)
{
	vod_halt(cls);
}

/* url: /vod/<pid>.ts?bill=<playbillID>&start=<unix s>&end=<unix s>
 *      &sid=<speed session>  (python /vod rules; end optional) */
static MHD_HANDLER_RET handle_vod(struct MHD_Connection *conn, const char *url)
{
	char pid[40];
	snprintf(pid, sizeof(pid), "%s", strrchr(url, '/') + 1);
	char *dot = strrchr(pid, '.');
	if (dot && strcmp(dot, ".ts") == 0)
		*dot = '\0';

	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0)
		return send_text(conn, "{\"error\":\"no such channel\"}",
				 MHD_HTTP_NOT_FOUND, "application/json");
	if (!ch.chid[0])
		return send_text(conn,
				 "{\"error\":\"channel has no EPG, TVOD unsupported\"}",
				 MHD_HTTP_BAD_REQUEST, "application/json");

	const char *bill = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "bill");
	const char *sid = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "sid");
	const char *a;
	int64_t start = 0, end = 0;
	if ((a = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "start")))
		start = strtoll(a, NULL, 10);
	if ((a = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "end")))
		end = strtoll(a, NULL, 10);
	if (!bill || !*bill || start <= 0)
		return send_text(conn, "{\"error\":\"missing bill or start\"}",
				 MHD_HTTP_BAD_REQUEST, "application/json");
	if (sid && !vod_sid_ok(sid))
		sid = "";

	char err[1024] = "getTvodPlayUrl failed";
	struct vod_stream *s = NULL;
	for (int attempt = 0; attempt < 2 && !s; attempt++) {
		char play_url[HTTPC_URL_MAX];
		if (epg_tvod_play_url(pid, bill, start, end, play_url,
				      sizeof(play_url)) == 0) {
			s = vod_open(play_url, sid, err, sizeof(err));
			if (!s)
				/* mint worked, media fetch didn't: not an
				 * auth-chain problem, retry won't help and
				 * a full EPG re-merge would stall this
				 * worker for minutes */
				break;
		} else {
			snprintf(err, sizeof(err), "getTvodPlayUrl failed for %s", pid);
			log_msg(LL_WARN, "vod: open failed (attempt %d): %s",
				attempt + 1, err);
			if (attempt == 0 && auth_mgr_enabled()) {
				/* session may have expired: force re-auth + reload */
				struct auth_state *st = malloc(sizeof(*st));
				if (st) {
					if (auth_mgr_force_refresh(st) == 0)
						epg_refresh_channels(st);
					free(st);
				}
				epg_find_channel(pid, &ch);
			}
		}
	}
	if (!s)
		return send_text(conn, err, MHD_HTTP_BAD_GATEWAY, "text/plain");

	/* 1.1: transcode AC-3 audio on the 4K VOD programs too (the HLS
	 * segments are HEVC+AC-3 for those, same MSE limitation as live) */
	struct xcode_sess *xc = NULL;
	if (CFG->xcode && ch.ac3) {
		struct xcode_src xs = {
			.read = vod_src_read,
			.halt = vod_src_halt,
			.free = vod_free,
			.src = s,
		};
		xc = xcode_open(&xs);
	}

	struct MHD_Response *r = MHD_create_response_from_callback(
		MHD_SIZE_UNKNOWN, 32 * 1024,
		xc ? xcode_read : vod_reader,
		xc ? (void *)xc : (void *)s,
		xc ? xcode_close : vod_free);
	if (!r) {
		if (xc)
			xcode_close(xc);
		else
			vod_close(s);
		return MHD_NO;
	}
	MHD_add_response_header(r, "Content-Type", "video/mp2t");
	MHD_add_response_header(r, "Cache-Control", "no-store");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	size_t bl = strlen(bill);
	log_msg(LL_INFO, "vod: client start %s bill=%.8s%s", pid,
		bl > 8 ? bill + bl - 8 : bill, xc ? " (ac3->aac)" : "");
	return ret;
}

/* -------- POST bodies (0.5: /api/vod_speed) -------- */

struct post_body {
	char b[256];
	size_t len;
};

/* frees an unfinished POST body when the connection dies mid-upload;
 * finished requests free it in the handler and reset *con_cls */
static void request_done(void *cls, struct MHD_Connection *conn,
			 void **con_cls, enum MHD_RequestTerminationCode toe)
{
	(void)cls; (void)conn; (void)toe;
	if (*con_cls) {
		free(*con_cls);
		*con_cls = NULL;
	}
}

/* %-decode + '+' -> ' ' in place */
static void url_decode(char *s)
{
	char *r = s;
	while (*s) {
		if (*s == '%' && s[1] && s[2]) {
			int hi = s[1], lo = s[2];
			hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10;
			lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10;
			if (hi >= 0 && hi <= 15 && lo >= 0 && lo <= 15) {
				*r++ = (char)(hi * 16 + lo);
				s += 3;
				continue;
			}
		}
		*r++ = (*s == '+') ? ' ' : *s;
		s++;
	}
	*r = '\0';
}

/* value of name in an application/x-www-form-urlencoded body (tiny
 * static buffer; copy out before the next call) */
static const char *form_value(const char *body, const char *name)
{
	static char val[128];
	size_t nl = strlen(name);
	for (const char *p = body; *p; ) {
		const char *amp = strchr(p, '&');
		size_t seg = amp ? (size_t)(amp - p) : strlen(p);
		if (seg > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
			size_t vl = seg - nl - 1;
			if (vl >= sizeof(val))
				vl = sizeof(val) - 1;
			memcpy(val, p + nl + 1, vl);
			val[vl] = '\0';
			url_decode(val);
			return val;
		}
		if (!amp)
			break;
		p = amp + 1;
	}
	return NULL;
}

/* POST /api/vod_speed  body: sid=<id>&speed=<x>  (python api_vod_speed) */
static MHD_HANDLER_RET handle_api_vod_speed(struct MHD_Connection *conn,
					    const char *upload_data,
					    size_t *upload_data_size,
					    void **con_cls)
{
	if (*con_cls == NULL) {
		struct post_body *pb = calloc(1, sizeof(*pb));
		if (!pb)
			return MHD_NO;
		*con_cls = pb;
		return MHD_YES;
	}
	struct post_body *pb = *con_cls;
	if (upload_data && *upload_data_size) {
		size_t take = *upload_data_size;
		if (take > sizeof(pb->b) - 1 - pb->len)
			take = sizeof(pb->b) - 1 - pb->len;
		memcpy(pb->b + pb->len, upload_data, take);
		pb->len += take;
		*upload_data_size = 0;
		return MHD_YES;
	}

	char sid[VOD_SID_MAX + 1] = "";
	const char *qa = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
						     "sid");
	const char *v = (qa && *qa) ? qa : form_value(pb->b, "sid");
	if (v)
		snprintf(sid, sizeof(sid), "%s", v);
	double speed = 1.0;
	if ((qa = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
					      "speed")) && *qa)
		speed = strtod(qa, NULL);
	else if ((v = form_value(pb->b, "speed")))
		speed = strtod(v, NULL);

	MHD_HANDLER_RET ret;
	char out[96];
	if (!vod_sid_ok(sid)) {
		ret = send_text(conn, "{\"error\":\"bad sid\"}",
				MHD_HTTP_BAD_REQUEST, "application/json");
	} else {
		vod_sess_set(sid, speed);
		snprintf(out, sizeof(out), "{\"ok\":true,\"speed\":%.2f}",
			 vod_sess_get(sid));
		ret = send_text(conn, out, MHD_HTTP_OK, "application/json");
	}
	free(pb);
	*con_cls = NULL;
	return ret;
}

static MHD_HANDLER_RET handle_api_ts_window(struct MHD_Connection *conn, const char *url)
{
	char pid[40];
	snprintf(pid, sizeof(pid), "%s", strrchr(url, '/') + 1);
	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0 || !ch.ts || !ch.timeshift_url[0])
		return send_text(conn, "{\"error\":\"no such channel\"}",
				 MHD_HTTP_NOT_FOUND, "application/json");

	char err[1024], cb[1024];
	if (ts_resolve(ch.timeshift_url, cb, sizeof(cb), err, sizeof(err)) != 0)
		return send_text(conn, err, MHD_HTTP_BAD_GATEWAY, "text/plain");
	int64_t s = -1, e = -1;
	ts_parse_window(cb, &s, &e);
	char out[160];
	snprintf(out, sizeof(out), "{\"pid\":%s,\"start\":%lld,\"end\":%lld}",
		 "null", (long long)s, (long long)e);
	return send_text(conn, out, MHD_HTTP_OK, "application/json");
}

/* -------- static web files (1.0) -------- */

#define WEB_ROOT "/usr/share/sh-telecom-iptv/web"

/* dev override so the frontend can be served straight from the tree */
static const char *web_root(void)
{
	const char *env = getenv("IPTVD_WEB");
	return env && *env ? env : WEB_ROOT;
}

static const char *web_mime(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot)
		return "application/octet-stream";
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(dot, ".js") == 0)
		return "application/javascript";
	if (strcmp(dot, ".css") == 0)
		return "text/css";
	if (strcmp(dot, ".svg") == 0)
		return "image/svg+xml";
	if (strcmp(dot, ".png") == 0)
		return "image/png";
	if (strcmp(dot, ".ico") == 0)
		return "image/x-icon";
	return "application/octet-stream";
}

/* serve WEB_ROOT/<name>; the caller only passes single-segment names,
 * so the path can never leave the web root.  MHD_NO = file absent. */
static MHD_HANDLER_RET handle_web(struct MHD_Connection *conn, const char *name)
{
	char path[160];
	snprintf(path, sizeof(path), "%s/%s", web_root(), name);
	size_t sz;
	char *buf = read_file(path, 2 * 1024 * 1024, &sz);
	if (!buf)
		return MHD_NO;
	struct MHD_Response *r = MHD_create_response_from_buffer(
		sz, buf, MHD_RESPMEM_MUST_FREE);
	if (!r) {
		free(buf);
		return MHD_NO;
	}
	int html = strncmp(name, "index.", 6) == 0;
	MHD_add_response_header(r, "Content-Type", web_mime(name));
	MHD_add_response_header(r, "Cache-Control",
				html ? "no-store" : "max-age=3600");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	return ret;
}

static MHD_HANDLER_RET handle_status(struct MHD_Connection *conn)
{
	char *buf = malloc(4096);
	if (!buf)
		return MHD_NO;
	char dev[40] = "(relay disabled)";
	if (CFG->device[0])
		snprintf(dev, sizeof(dev), "%s%s", CFG->device,
			 mcast_ready() ? " [up]" : " [waiting]");

	char auth[96] = "disabled";
	if (auth_mgr_enabled()) {
		/* heap: struct auth_state ~104KB vs musl 128KB worker stacks */
		struct auth_state *a = malloc(sizeof(*a));
		if (a) {
			auth_mgr_snapshot(a);
			if (a->ok)
				snprintf(auth, sizeof(auth), "ok (age %lds, %s)",
					 (long)(time(NULL) - a->auth_time),
					 a->epg_host_url[0] ? a->epg_host_url : "-");
			else if (a->err[0])
				snprintf(auth, sizeof(auth), "failed: %.60s", a->err);
			else
				snprintf(auth, sizeof(auth), "starting");
			free(a);
		}
	}

	long up_s = (long)((util_now_ms() - started_ms) / 1000);
	snprintf(buf, 4096,
		 "iptvd " IPTVD_VERSION " (udpxy-compatible multicast relay)\n"
		 "uptime: %ldd %02ld:%02ld:%02ld\n"
		 "source device: %s\n"
		 "active groups: %d\n"
		 "active clients: %d (max %d)\n"
		 "rtp mode: %s\n"
		 "rtp reorder: %s\n"
		 "fcc: %s\n"
		 "auth: %s\n"
		 "epg: %d channels%s\n",
		 up_s / 86400, (up_s / 3600) % 24, (up_s / 60) % 60, up_s % 60,
		 dev,
		 mcast_group_count(),
		 mcast_client_count(), CFG->max_clients,
		 CFG->rtp_mode == 0 ? "auto" : CFG->rtp_mode == 1 ? "always" : "never",
		 CFG->reorder ? "on" : "off",
		 fcc_enabled() ? "on (fast channel change)" : "off",
		 auth,
		 epg_channel_count(),
		 auth_mgr_enabled() ? "" : " (auth off)");
	struct MHD_Response *r = MHD_create_response_from_buffer(
		strlen(buf), buf, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(r, "Content-Type", "text/plain");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	return ret;
}

static MHD_HANDLER_RET handle_playlist_static(struct MHD_Connection *conn)
{
	size_t sz;
	char *buf = read_file(PLAYLIST_PATH, 4 * 1024 * 1024, &sz);
	if (!buf)
		return send_text(conn,
				 "no channels.m3u installed at " PLAYLIST_PATH "\n",
				 MHD_HTTP_NOT_FOUND, "text/plain");
	struct MHD_Response *r = MHD_create_response_from_buffer(
		sz, buf, MHD_RESPMEM_MUST_FREE);
	MHD_add_response_header(r, "Content-Type", "audio/x-mpegurl");
	int ret = MHD_queue_response(conn, MHD_HTTP_OK, r);
	MHD_destroy_response(r);
	return ret;
}

/* -------- MHD callback -------- */

static MHD_HANDLER_RET ahc(void *cls, struct MHD_Connection *conn, const char *url,
	       const char *method, const char *version,
	       const char *upload_data, size_t *upload_data_size,
	       void **con_cls)
{
	(void)cls; (void)version;
	/* POST is allowed only where a body is expected (0.5 speed API) */
	if (strcmp(method, "POST") == 0 && strcmp(url, "/api/vod_speed") == 0)
		return handle_api_vod_speed(conn, upload_data,
					    upload_data_size, con_cls);
	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
		return send_text(conn, "method not allowed\n",
				 MHD_HTTP_METHOD_NOT_ALLOWED, "text/plain");

	if (strcmp(url, "/status") == 0 || strcmp(url, "/status/") == 0)
		return handle_status(conn);
	if (strcmp(url, "/playlist.m3u") == 0)
		return handle_playlist(conn);
	if (strcmp(url, "/api/channels") == 0)
		return handle_api_channels(conn);
	if (strncmp(url, "/api/programs/", 14) == 0)
		return handle_api_programs(conn, url);
	if (strncmp(url, "/api/now/", 9) == 0)
		return handle_api_now(conn, url);
	if (strcmp(url, "/api/search") == 0)
		return handle_api_search(conn);
	if (strncmp(url, "/ts/", 4) == 0)
		return handle_ts(conn, url);
	if (strncmp(url, "/api/ts_window/", 15) == 0)
		return handle_api_ts_window(conn, url);
	if (strncmp(url, "/vod/", 5) == 0)
		return handle_vod(conn, url);
	if (!strncmp(url, "/udp/", 5) || !strncmp(url, "/rtp/", 5))
		return handle_relay(conn, url);

	/* 1.0 web UI: "/" -> index.html, other single-segment paths are
	 * static assets served from the web root */
	if (strcmp(url, "/") == 0) {
		MHD_HANDLER_RET r = handle_web(conn, "index.html");
		if (r != MHD_NO)
			return r;
		return handle_index(conn);  /* web root not installed */
	}
	if (url[0] == '/' && url[1] && strchr(url + 1, '/') == NULL) {
		MHD_HANDLER_RET r = handle_web(conn, url + 1);
		if (r != MHD_NO)
			return r;
		return send_text(conn, "not found\n", MHD_HTTP_NOT_FOUND,
				 "text/plain");
	}
	return handle_index(conn);
}

static MHD_HANDLER_RET handle_index(struct MHD_Connection *conn)
{
	const char *html =
		"<html><head><title>iptvd</title></head><body>"
		"<h3>iptvd " IPTVD_VERSION "</h3>"
		"<p>udpxy-compatible multicast relay</p>"
		"<p><a href='/status'>/status</a> &middot; "
		"<a href='/playlist.m3u'>/playlist.m3u</a> &middot; "
		"<a href='/api/channels'>/api/channels</a> &middot; "
		"/udp/&lt;group&gt;:&lt;port&gt; &middot; /ts/&lt;pid&gt;.ts &middot; "
		"/vod/&lt;pid&gt;.ts</p>"
		"<p>(web UI not installed at " WEB_ROOT ")</p>"
		"</body></html>";
	return send_text(conn, html, MHD_HTTP_OK, "text/html");
}

static volatile int g_stop;

int httpd_run(struct iptv_cfg *cfg)
{
	CFG = cfg;
	started_ms = util_now_ms();

	struct sockaddr_in laddr;
	memset(&laddr, 0, sizeof(laddr));
	laddr.sin_family = AF_INET;
	laddr.sin_port = htons((unsigned short)cfg->port);
	laddr.sin_addr.s_addr = inet_addr(cfg->bind);
	if (laddr.sin_addr.s_addr == (in_addr_t)-1)
		laddr.sin_addr.s_addr = INADDR_ANY;

	struct MHD_Daemon *d = MHD_start_daemon(
		MHD_USE_THREAD_PER_CONNECTION, (unsigned short)cfg->port,
		NULL, NULL, ahc, NULL,
		MHD_OPTION_SOCK_ADDR, &laddr,
		MHD_OPTION_NOTIFY_COMPLETED, request_done, NULL,
		MHD_OPTION_END);
	if (!d) {
		log_msg(LL_ERR, "httpd: cannot listen on %s:%d",
			cfg->bind, cfg->port);
		return -1;
	}
	log_msg(LL_INFO, "httpd: listening on %s:%d", cfg->bind, cfg->port);

	/* MHD_stop_daemon blocks until streaming clients disconnect;
	 * procd escalates to SIGKILL after its timeout, which is fine. */
	while (!g_stop)
		sleep(1);
	MHD_stop_daemon(d);
	return 0;
}

void httpd_stop(void)
{
	g_stop = 1;
}
