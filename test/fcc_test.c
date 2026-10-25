/* Host-side test for the 0.6 FCC client: wire-format vectors plus an
 * end-to-end round against the local UDP fixture (FMT2 -> FMT3 ->
 * accelerated RTP burst -> FMT4 -> FMT5).
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/fcc_test \
 *       test/fcc_test.c src/fcc.c src/log.c src/util.c -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fcc.h"
#include "iptv.h"

static int fails;
static int pid_dummy;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-32s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

static void test_vectors(void)
{
	unsigned char b[40];
	in_addr_t group = inet_addr("225.1.1.1");

	/* FMT=2: V2|FMT2, PT=205, len=9, sender 0, media=group IP,
	 * FCI ver 0 + reserved, client port, group port, group IP, zeros */
	memset(b, 0xAA, sizeof(b));
	int n = fcc_build_fmt2(b, group, 5000, 6000);
	unsigned char want2[40] = { 0 };
	want2[0] = 0x82;
	want2[1] = 205;
	want2[3] = 9;
	want2[8] = 225; want2[9] = 1; want2[10] = 1; want2[11] = 1;
	want2[16] = 6000 >> 8; want2[17] = 6000 & 0xFF;
	want2[18] = 5000 >> 8; want2[19] = 5000 & 0xFF;
	want2[20] = 225; want2[21] = 1; want2[22] = 1; want2[23] = 1;
	CHECK("fmt2 size", n == 40);
	CHECK("fmt2 bytes", memcmp(b, want2, 40) == 0);

	/* FMT=5: V2|FMT5, PT=205, len=3, media=group IP, stop 0, mseq */
	memset(b, 0xAA, sizeof(b));
	n = fcc_build_fmt5(b, group, 0, 1234);
	unsigned char want5[16] = { 0 };
	want5[0] = 0x85;
	want5[1] = 205;
	want5[3] = 3;
	want5[8] = 225; want5[9] = 1; want5[10] = 1; want5[11] = 1;
	want5[14] = 1234 >> 8; want5[15] = 1234 & 0xFF;
	CHECK("fmt5 size", n == 16);
	CHECK("fmt5 bytes", memcmp(b, want5, 16) == 0);

	unsigned char b2[16];
	fcc_build_fmt5(b2, group, 1, 0);
	CHECK("fmt5 force flag", b2[12] == 1);
}

int main(void)
{
	test_vectors();

	const int port = 18945;
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "python3 test/fixtures/fcc_server.py %d >/tmp/fcc_fixture.log 2>&1 & echo $!",
		 port);
	FILE *pf = popen(cmd, "r");
	if (!pf || fscanf(pf, "%d", &pid_dummy) != 1)
		return 1;
	pclose(pf);
	usleep(300000);

	struct iptv_cfg cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.fcc = 1;
	snprintf(cfg.device, sizeof(cfg.device), "%s", "lo");
	snprintf(cfg.fcc_srv[0], sizeof(cfg.fcc_srv[0]),
		 "127.0.0.1:%d", port);
	cfg.n_fcc_srv = 1;
	fcc_init(&cfg);
	CHECK("fcc enabled", fcc_enabled() && fcc_server_count() == 1);

	/* r16: per-channel override server path (single target) */
	struct fcc_sess *s = fcc_start(inet_addr("225.1.1.1"), 5000,
				       "127.0.0.1:18945");
	CHECK("session started", s != NULL);
	if (!s)
		return 1;
	CHECK("fd valid", fcc_fd(s) >= 0);
	CHECK("requesting", fcc_pending(s));
	CHECK("not stalled", !fcc_stalled(s));

	/* drive the session: FMT3, 5 media packets, FMT4 */
	int media = 0, sync = 0, resp = 0, bad_payload = 0;
	uint16_t last_seq = 0;
	for (int i = 0; i < 300 && !sync; i++) {
		struct pollfd pfd = { .fd = fcc_fd(s), .events = POLLIN };
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		unsigned char buf[65536];
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		int n = recvfrom(fcc_fd(s), buf, sizeof(buf), 0,
				 (struct sockaddr *)&from, &flen);
		if (n <= 0)
			continue;
		int rc = fcc_pkt(s, buf, n, &from);
		if (rc == FCC_PKT_RESP) {
			resp++;
			CHECK("winner known", !fcc_pending(s));
		} else if (rc == FCC_PKT_MEDIA) {
			/* RTP MP2T: V2 PT33, payload = 7x188 of the seq byte */
			uint16_t seq = ((uint16_t)buf[2] << 8) | buf[3];
			if (media > 0 && seq != (uint16_t)(last_seq + 1))
				bad_payload |= 2;
			last_seq = seq;
			if ((buf[0] >> 6) != 2 || (buf[1] & 0x7f) != 33 ||
			    n != 12 + 7 * 188)
				bad_payload |= 1;
			for (int k = 12; k < n; k++)
				if (buf[k] != (unsigned char)(seq & 0xFF)) {
					bad_payload |= 4;
					break;
				}
			media++;
		} else if (rc == FCC_PKT_SYNC) {
			sync = 1;
		}
	}
	CHECK("fmt3 response", resp == 1);
	CHECK("media burst", media == 5);
	CHECK("media order/pattern", bad_payload == 0 && last_seq == 104);
	CHECK("sync received", sync == 1);

	/* r14: the stall detector must track media arrival, not the FMT=3
	 * accept time.  The paced burst spans ~1.2s since accept but the
	 * last media is fresh, so no stall here (old code failed this). */
	CHECK("stall tracks media (r14)", !fcc_stalled(s));
	usleep(1100000);                   /* > FCC_STALL_MS of silence */
	CHECK("stall fires after silence", fcc_stalled(s));

	fcc_finish(s, 0, 0);          /* fixture asserts the FMT=5 shape */

	kill(pid_dummy, SIGTERM);
	waitpid(pid_dummy, NULL, 0);
	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
