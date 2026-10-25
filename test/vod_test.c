/* Host-side test for the 0.5 TVOD module: playlist parsing units +
 * speed-session units + an end-to-end chain (auth fixture ->
 * getTvodPlayUrl -> 302 -> master playlist -> media playlist with a
 * 404 segment -> concatenated TS bytes).
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/vod_test \
 *       test/vod_test.c src/vod.c src/epg.c src/json.c src/auth.c \
 *       src/httpc.c src/log.c src/util.c src/crypto/md5.c \
 *       src/crypto/aes.c -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "auth.h"
#include "epg.h"
#include "vod.h"

static int fails;
static int pid_dummy;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-32s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

static void sl_free(struct vod_seglist *sl)
{
	free(sl->pool);
	sl->pool = NULL;
	sl->pool_len = sl->pool_cap = 0;
	sl->n = 0;
}

static void test_parse(void)
{
	struct vod_seglist sl;
	char child[512];

	/* master playlist: variant named, no segments */
	memset(&sl, 0, sizeof(sl));
	int n = vod_parse_playlist(
		"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nmedia/med.m3u8\n",
		"http://h:1/tvod/pl.m3u8", &sl, child, sizeof(child));
	CHECK("master names variant", n == 0 && sl.n == 0 &&
	      strcmp(child, "media/med.m3u8") == 0);
	sl_free(&sl);

	/* media playlist: absolute + relative segments, EXTINF/default
	 * durations, comment/blank lines ignored */
	memset(&sl, 0, sizeof(sl));
	n = vod_parse_playlist(
		"#EXTM3U\n"
		"#EXT-X-TARGETDURATION:10\n"
		"\n"
		"#EXTINF:6.0,\n"
		"http://s:2/a/seg1.ts\n"
		"seg2.ts\n"
		"#EXTINF:garbage,\n"
		"sub/seg3.ts\n",
		"http://s:2/a/med.m3u8", &sl, child, sizeof(child));
	CHECK("media count", n == 3 && sl.n == 3);
	CHECK("no variant for media", child[0] == '\0');
	CHECK("absolute kept", strcmp(sl.pool + sl.off[0],
				      "http://s:2/a/seg1.ts") == 0);
	CHECK("relative joined", strcmp(sl.pool + sl.off[1],
					"http://s:2/a/seg2.ts") == 0);
	CHECK("subdir relative joined", strcmp(sl.pool + sl.off[2],
					       "http://s:2/a/sub/seg3.ts") == 0);
	CHECK("extinf duration", sl.dur_ms[0] == 6000);
	CHECK("default duration", sl.dur_ms[1] == 10000);
	CHECK("bad extinf -> default", sl.dur_ms[2] == 10000);
	sl_free(&sl);

	/* real-world long segments: per-segment URLs carry ~700B of
	 * AuthInfo/params (measured ~907B on SHDXFH_live media playlists,
	 * 642 segments / 593KB total) - must survive the line buffer and
	 * URL joins untruncated */
	memset(&sl, 0, sizeof(sl));
	char longurl[1024];
	strcpy(longurl, "http://124.75.27.6:8006/SHDXFH_live/00000001000000050000000006000105/");
	strcat(longurl, "segment-00000000040044091932-1.ts?AuthInfo=");
	while (strlen(longurl) < 907)
		strcat(longurl, "R%2B4KP9s5orTs8Zw");
	strcat(longurl, "&citycode=21&usercode=75338202@etv1");
	char pl[1200];
	snprintf(pl, sizeof(pl), "#EXTM3U\n#EXTINF:10.0,\n%s\n", longurl);
	n = vod_parse_playlist(pl, "http://124.75.27.6:8006/x/index.m3u8",
			       &sl, child, sizeof(child));
	CHECK("long url count", n == 1 && sl.n == 1);
	CHECK("long url untruncated", strlen(sl.pool + sl.off[0]) == strlen(longurl) &&
	      strcmp(sl.pool + sl.off[0], longurl) == 0);
	sl_free(&sl);
}

static void test_sessions(void)
{
	CHECK("sid alnum ok", vod_sid_ok("abc123XYZ") == 1);
	CHECK("sid empty no", vod_sid_ok("") == 0);
	CHECK("sid punct no", vod_sid_ok("a-b") == 0);
	CHECK("sid long no", vod_sid_ok("123456789012345678901234567890"
					"123456789012345678901234567890"
					"12345") == 0);

	CHECK("unknown sid speed 1.0", vod_sess_get("nosuch") == 1.0);
	CHECK("set ok", vod_sess_set("s1", 2.0) == 0);
	CHECK("clamp high", (vod_sess_set("s2", 9.0), vod_sess_get("s2") == 4.0));
	CHECK("clamp low", (vod_sess_set("s3", 0.01), vod_sess_get("s3") == 0.25));
	CHECK("round 2 decimals", (vod_sess_set("s4", 1.236),
				   vod_sess_get("s4") == 1.24));
	CHECK("read back", vod_sess_get("s1") == 2.0);
}

int main(void)
{
	test_parse();
	test_sessions();

	/* -------- end-to-end against the fixture -------- */
	const int port = 18943;
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "python3 test/fixtures/auth_server.py %d >/tmp/vod_fixture.log 2>&1 & echo $!",
		 port);
	FILE *pf = popen(cmd, "r");
	if (!pf || fscanf(pf, "%d", &pid_dummy) != 1)
		return 1;
	pclose(pf);
	sleep(1);

	struct iptv_cfg cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.userid, sizeof(cfg.userid), "%s", "12345678@etv1");
	snprintf(cfg.sn, sizeof(cfg.sn), "%s", "SNCZ1234567");
	snprintf(cfg.mac, sizeof(cfg.mac), "%s", "AA:BB:CC:DD:EE:FF");
	snprintf(cfg.auth_host, sizeof(cfg.auth_host), "127.0.0.1:%d", port);

	CHECK("auth manager start", auth_mgr_start(&cfg) == 0);
	struct auth_state st;
	for (int i = 0; i < 100; i++) {
		auth_mgr_snapshot(&st);
		if (st.ok)
			break;
		usleep(100000);
	}
	CHECK("auth chain ok", st.ok);
	CHECK("channel table merged", epg_refresh_channels(&st) == 3);

	CHECK("tvod url bad pid", epg_tvod_play_url("424242", "pb-1", 1000,
						    2000, cmd, sizeof(cmd)) == -1);
	char url[512];
	CHECK("getTvodPlayUrl", epg_tvod_play_url("1346060", "pb-1", 1000,
						  2000, url, sizeof(url)) == 0);
	CHECK("playURL shape", strstr(url, "/tvod/entry.m3u8") != NULL);

	char err[192];
	struct vod_stream *v = vod_open(url, "", err, sizeof(err));
	CHECK("vod open", v != NULL);
	if (v) {
		/* seg1 (Content-Length) + seg2 (404, skipped) + seg3 (chunked),
		 * concatenated in order.  r6: seg1's body carries one broken
		 * 0xff packet that vod_read must scrub into a null packet. */
		static const unsigned char pkt_a[188] = { 0x47, 0x10, 0x22, 0x10 };
		static const unsigned char pkt_b[188] = { 0x47, 0x10, 0x23, 0x10 };
				unsigned char null_pkt[188];
		memset(null_pkt, 0xff, sizeof(null_pkt));
		null_pkt[0] = 0x47; null_pkt[1] = 0x1f;
		null_pkt[2] = 0xff; null_pkt[3] = 0x10;
		char want[188 * 251];
		size_t wl = 0;
		for (int i = 0; i < 200; i++) {
			memcpy(want + wl, pkt_a, 188);
			wl += 188;
		}
		memcpy(want + wl, null_pkt, 188);
		wl += 188;
		for (int i = 0; i < 50; i++) {
			memcpy(want + wl, pkt_b, 188);
			wl += 188;
		}
		static char got[188 * 251 + 512];
		size_t gl = 0;
		int r;
		while ((r = vod_read(v, got + gl, sizeof(got) - gl)) > 0)
			gl += (size_t)r;
		CHECK("stream ended", r == -1);
		CHECK("concat length", gl == wl);
		CHECK("concat bytes", memcmp(got, want, gl) == 0);
		vod_close(v);
	}

	kill(pid_dummy, SIGTERM);
	waitpid(pid_dummy, NULL, 0);
	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
