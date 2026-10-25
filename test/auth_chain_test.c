/* Host-side end-to-end test of auth_run_full against the local fixture
 * server (test/fixtures/auth_server.py), which mimics the Shanghai
 * Telecom chain including a 302 redirect, a chunked body and a decoy
 * Set-Cookie.
 *
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/auth_chain_test \
 *       test/auth_chain_test.c src/auth.c src/httpc.c src/log.c \
 *       src/util.c src/crypto/md5.c src/crypto/aes.c -lpthread
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "auth.h"

static int fails;

#define CHECK(name, cond) do { \
	int ok_ = (cond); \
	printf("%-28s %s\n", name, ok_ ? "PASS" : "FAIL"); \
	if (!ok_) fails++; \
} while (0)

int main(void)
{
	const int port = 18923;
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "python3 test/fixtures/auth_server.py %d >/tmp/auth_fixture.log 2>&1 & echo $!",
		 port);
	FILE *pf = popen(cmd, "r");
	if (!pf)
		return 1;
	int pid = 0;
	if (fscanf(pf, "%d", &pid) != 1)
		return 1;
	pclose(pf);
	sleep(1);                       /* let the fixture listen */

	struct iptv_cfg cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.userid, sizeof(cfg.userid), "%s", "12345678@etv1");
	snprintf(cfg.sn, sizeof(cfg.sn), "%s", "SNCZ1234567");
	snprintf(cfg.mac, sizeof(cfg.mac), "%s", "AA:BB:CC:DD:EE:FF");
	snprintf(cfg.auth_host, sizeof(cfg.auth_host), "127.0.0.1:%d", port);

	struct auth_state st;
	int rc = auth_run_full(&cfg, "30.170.1.2", &st);

	CHECK("chain rc", rc == 0);
	CHECK("usertoken", strcmp(st.usertoken, "UT-4321") == 0);
	CHECK("epggroup", strcmp(st.epggroup, "EG-77") == 0);
	CHECK("session_id", strcmp(st.session_id, "S-777") == 0);
	char want_url[256];
	snprintf(want_url, sizeof(want_url), "http://127.0.0.1:%d/iptvepg/F-42", port);
	CHECK("epg_host_url", strcmp(st.epg_host_url, want_url) == 0);
	CHECK("bims_user_token", strcmp(st.bims_user_token, "BIMS-TOKEN-1") == 0);
	CHECK("ok flag", st.ok == 1);

	/* failure path: unreachable auth server must set err, not crash */
	snprintf(cfg.auth_host, sizeof(cfg.auth_host), "127.0.0.1:1");
	struct auth_state st2;
	int rc2 = auth_run_full(&cfg, "30.170.1.2", &st2);
	CHECK("unreachable rc", rc2 != 0);
	CHECK("unreachable err", st2.err[0] != '\0' && st2.ok == 0);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
