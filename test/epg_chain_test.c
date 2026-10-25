/* Host-side end-to-end test: auth chain + epg channel merge + program
 * guide + now/next + search index against the local fixture server.
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/epg_chain_test \
 *       test/epg_chain_test.c src/epg.c src/json.c src/auth.c \
 *       src/httpc.c src/log.c src/util.c src/crypto/md5.c \
 *       src/crypto/aes.c -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#include "auth.h"
#include "epg.h"

static int fails;
static int pid_dummy;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-32s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

int main(void)
{
	const int port = 18941;
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "python3 test/fixtures/auth_server.py %d >/tmp/epg_fixture.log 2>&1 & echo $!",
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

	/* auth manager runs the chain; epg fetches snapshot it */
	CHECK("auth manager start", auth_mgr_start(&cfg) == 0);
	struct auth_state st;
	for (int i = 0; i < 100; i++) {
		auth_mgr_snapshot(&st);
		if (st.ok)
			break;
		usleep(100000);
	}
	CHECK("auth chain ok", st.ok);
	CHECK("raw channels parsed", st.nraw == 2);

	epg_mgr_start();
	int n = epg_refresh_channels(&st);
	/* epg7-merged + ottauth-only leftover + hand-verified static ch */
	CHECK("merge count (epg7+leftover+static)", n == 3);

	struct epg_channel ch;
	CHECK("find merged channel", epg_find_channel("1346060", &ch) == 0);
	CHECK("merged name", strcmp(ch.name, "东方卫视HD") == 0);
	CHECK("merged no/ts/hd", ch.no == 1 && ch.ts == 1 && ch.hd == 1);
	CHECK("merged chid", strcmp(ch.chid, "ch-1") == 0);
	CHECK("merged igmp", strcmp(ch.igmp, "igmp://225.1.1.1:5000") == 0);
	CHECK("merged fcc (r16)", strcmp(ch.fcc, "124.75.25.214:7777") == 0);
	{
		char fs[48];
		CHECK("fcc for group hit (r16)",
		     epg_fcc_for_group(inet_addr("225.1.1.1"), 5000,
				       fs, sizeof(fs)) == 1 &&
		     strcmp(fs, "124.75.25.214:7777") == 0);
		CHECK("fcc for group miss (r16)",
		     epg_fcc_for_group(inet_addr("225.1.1.2"), 5000,
				       fs, sizeof(fs)) == 0);
	}
	CHECK("merged category", strcmp(ch.cats, "卫视") == 0);
	CHECK("long pid matched", ch.code[0] != '\0');

	struct epg_channel ch2;
	CHECK("find leftover channel",
	      epg_find_channel("0000000006000130", &ch2) == 0);
	CHECK("leftover named by pid table", strcmp(ch2.name, "北京卫视4K") == 0);
	CHECK("leftover no", ch2.no == 9001);
	CHECK("leftover ac3 + 4K cat", ch2.ac3 == 1 && strcmp(ch2.cats, "4K") == 0);

	const struct epg_prog *progs;
	n = epg_programs("1346060", &progs);
	CHECK("programs fetched", n == 2);
	if (n >= 2) {
		CHECK("program dedup", strcmp(progs[0].name, "新闻") == 0 &&
		      strcmp(progs[1].name, "天气预报") == 0);
		CHECK("program times", progs[0].start_ms == 1000 &&
		      progs[0].end_ms == 2000);
	}
	n = epg_programs("1346060", &progs);
	CHECK("programs served from cache", n == 2);
	CHECK("unknown channel", epg_programs("424242", &progs) == -1);

	char *now = epg_now_next("1346060");
	CHECK("now/next passthrough", now && strstr(now, "now playing"));
	free(now);

	/* background search index over the merged table */
	int building = 0;
	int64_t age = -1;
	for (int i = 0; i < 100; i++) {
		epg_search_state(&building, &age);
		if (!building && age >= 0)
			break;
		usleep(100000);
	}
	CHECK("search index built", age >= 0);
	char *res = epg_search("新闻");
	CHECK("search hit", res && strstr(res, "新闻") && strstr(res, "东方卫视HD"));
	free(res);
	res = epg_search("不存在的节目xyz");
	CHECK("search miss empty", res && strcmp(res, "[]") == 0);
	free(res);

	kill(pid_dummy, SIGTERM);
	waitpid(pid_dummy, NULL, 0);
	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
