/* iptvd - IPTV daemon entry point (procd foreground process). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include "iptv.h"
#include "log.h"
#include "config.h"
#include "mcast.h"
#include "fcc.h"
#include "httpd.h"
#include "auth.h"
#include "epg.h"

static int g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
	httpd_stop();
}

/* The IPTV interface may not exist yet at boot (netifd brings up the
 * DHCP client asynchronously); keep retrying in the background. */
static void *relay_init_thread(void *arg)
{
	struct iptv_cfg *cfg = arg;
	while (!g_stop) {
		if (mcast_init(cfg->device, cfg->bind_addr, cfg->rtp_mode,
			       cfg->reorder) == 0) {
			fcc_init(cfg);   /* re-evaluate once the device is up */
			return NULL;
		}
		sleep(5);
	}
	return NULL;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"%s [-c conffile] [-p port] [-d]  (config: /etc/config/sh-telecom-iptv)\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *conf = "/etc/config/sh-telecom-iptv";
	int debug = 0;
	int opt;

	while ((opt = getopt(argc, argv, "c:p:dh")) != -1) {
		switch (opt) {
		case 'c': conf = optarg; break;
		case 'p': break; /* port override comes via UCI; kept for future use */
		case 'd': debug = 1; break;
		default: usage(argv[0]); return 2;
		}
	}

	log_init("iptvd");
	struct iptv_cfg cfg;
	if (config_load(conf, &cfg) != 0) {
		log_msg(LL_ERR, "cannot load config, exiting");
		return 1;
	}
	if (debug)
		cfg.log_level = LL_DEBUG;
	log_level(cfg.log_level);

	if (cfg.disabled) {
		log_msg(LL_INFO, "disabled in UCI, exiting");
		return 0;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (cfg.device[0]) {
		mcast_init(cfg.device, cfg.bind_addr, cfg.rtp_mode, cfg.reorder);
		fcc_init(&cfg);
	}

	pthread_t th;
	if (cfg.device[0] && !mcast_ready()) {
		pthread_create(&th, NULL, relay_init_thread, &cfg);
		pthread_detach(th);
	}

	/* auth chain runs when the account section is filled in */
	auth_mgr_start(&cfg);
	if (auth_mgr_enabled())
		epg_mgr_start();

	log_msg(LL_INFO, "iptvd %s starting", IPTVD_VERSION);

	return httpd_run(&cfg) == 0 ? 0 : 1;
}
