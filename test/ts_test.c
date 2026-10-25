/* Host-side test for src/ts.c against the local RTSP fixture.
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/ts_test \
 *       test/ts_test.c src/ts.c src/log.c -lpthread
 *   (run from package/iptv; starts test/fixtures/rtsp_server.py itself)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "ts.h"

static int fails;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-32s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

int main(void)
{
	const int port = 18955;
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "python3 test/fixtures/rtsp_server.py %d >/tmp/rtsp_fixture.log 2>&1 & echo $!",
		 port);
	FILE *pf = popen(cmd, "r");
	int pid = 0;
	if (!pf || fscanf(pf, "%d", &pid) != 1)
		return 1;
	pclose(pf);
	sleep(1);

	/* window parser unit */
	int64_t ws = -1, we = -1;
	int has = ts_parse_window("rtsp://h/LIVE/1?AuthInfo=x&DURATION=20260101T000000Z-20260101T040000Z",
				  &ws, &we);
	CHECK("window parsed", has == 1);
	CHECK("window start epoch", ws == 1767225600);
	CHECK("window end epoch", we == 1767240000);
	has = ts_parse_window("rtsp://h/LIVE/1?AuthInfo=x", &ws, &we);
	CHECK("window absent", has == 0);

	/* resolve: 302 chain -> content-base */
	char err[192], cb[1024];
	char entry[256];
	snprintf(entry, sizeof(entry), "rtsp://127.0.0.1:%d/entry", port);
	int rc = ts_resolve(entry, cb, sizeof(cb), err, sizeof(err));
	CHECK("resolve rc", rc == 0);
	CHECK("content-base AuthInfo", strstr(cb, "AuthInfo=abc") != NULL);
	CHECK("content-base DURATION", strstr(cb, "DURATION=20260101") != NULL);

	/* open + read: three 188-byte TS frames in order */
	struct ts_stream *s = ts_open(cb, 0, 0, err, sizeof(err));
	CHECK("open rc", s != NULL);

	unsigned char buf[2048];
	int total = 0, bad = 0;
	while (total < 3 * 188) {
		int n = ts_read(s, buf + total, sizeof(buf) - (size_t)total);
		if (n < 0)
			break;
		total += n;
	}
	CHECK("frame bytes total", total == 3 * 188);
	for (int i = 0; i < 3; i++) {
		unsigned char *p = buf + i * 188;
		if (p[0] != 0x47 || p[1] != i || p[2] != i % 251 || p[187] != i % 251)
			bad = 1;
	}
	CHECK("frame contents in order", bad == 0);

	/* small-buffer reads exercise the mid-frame split path */
	struct ts_stream *s2 = ts_open(cb, 0, 0, err, sizeof(err));
	CHECK("reopen", s2 != NULL);
	unsigned char small[100];
	total = 0;
	unsigned char flat[3 * 188];
	while (total < 3 * 188) {
		int n = ts_read(s2, small, sizeof(small));
		if (n <= 0)
			break;
		memcpy(flat + total, small, (size_t)n);
		total += n;
	}
	CHECK("split reads total", total == 3 * 188);
	bad = 0;
	for (int i = 0; i < 3; i++) {
		unsigned char *p = flat + i * 188;
		if (p[0] != 0x47 || p[1] != i)
			bad = 1;
	}
	CHECK("split reads contents", bad == 0);
	ts_close(s);
	ts_close(s2);

	/* absolute replay point: fixture asserts Range: clock= presence */
	snprintf(cb, sizeof(cb),
		 "rtsp://127.0.0.1:%d/LIVE/77?AuthInfo=abc&DURATION=%s",
		 port, "20260101T000000Z-20260101T040000Z");
	struct ts_stream *s3 = ts_open(cb, 1767225600 + 600, 0, err, sizeof(err));
	CHECK("open with Range", s3 != NULL);
	if (s3) {
		int n = ts_read(s3, buf, sizeof(buf));
		CHECK("range stream reads", n > 0 && buf[0] == 0x47);
		ts_close(s3);
	}

	/* failure path: unreachable server */
	rc = ts_resolve("rtsp://127.0.0.1:1/entry", cb, sizeof(cb), err, sizeof(err));
	CHECK("unreachable fails", rc != 0 && err[0] != '\0');

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
