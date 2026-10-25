/* UCI configuration loader (/etc/config/sh-telecom-iptv).
 *
 * libuci API used here is stable from OpenWrt 19.07 through 25.12.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <uci.h>

#include "iptv.h"
#include "config.h"
#include "log.h"

static const char *opt_str(struct uci_context *ctx, struct uci_section *s,
			   const char *name)
{
	return uci_lookup_option_string(ctx, s, name);
}

static int opt_int(struct uci_context *ctx, struct uci_section *s,
		   const char *name, int deflt)
{
	const char *v = uci_lookup_option_string(ctx, s, name);
	if (!v || !*v)
		return deflt;
	return atoi(v);
}

static void opt_bool(struct uci_context *ctx, struct uci_section *s,
		     const char *name, int *out)
{
	const char *v = uci_lookup_option_string(ctx, s, name);
	if (!v)
		return;
	*out = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
		strcmp(v, "on") == 0 || strcmp(v, "yes") == 0);
}

static void set_str(char *dst, size_t cap, const char *src)
{
	/* an empty option value keeps the built-in default */
	if (src && *src)
		snprintf(dst, cap, "%s", src);
}

int config_load(const char *path, struct iptv_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->port = 4022;
	snprintf(cfg->bind, sizeof(cfg->bind), "0.0.0.0");
	cfg->max_clients = 20;
	cfg->rtp_mode = 0; /* auto */
	cfg->reorder = 1;
	cfg->log_level = LL_INFO;
	cfg->xcode = 1;   /* AC-3 transcode on by default; harmless without ffmpeg */
	snprintf(cfg->auth_host, sizeof(cfg->auth_host), "222.68.208.73:7001");

	(void)path; /* the daemon always reads the standard /etc/config/sh-telecom-iptv */

	struct uci_context *ctx = uci_alloc_context();
	if (!ctx)
		return -1;

	struct uci_package *pkg = NULL;
	if (uci_load(ctx, "sh-telecom-iptv", &pkg) != 0 || !pkg) {
		log_msg(LL_ERR, "config: cannot load UCI package 'sh-telecom-iptv'");
		uci_free_context(ctx);
		return -1;
	}

	struct uci_element *e;
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		if (strcmp(s->type, "main") == 0) {
			opt_bool(ctx, s, "disabled", &cfg->disabled);
			cfg->port = opt_int(ctx, s, "port", cfg->port);
			set_str(cfg->bind, sizeof(cfg->bind), opt_str(ctx, s, "bind"));
			set_str(cfg->device, sizeof(cfg->device), opt_str(ctx, s, "device"));
			set_str(cfg->bind_addr, sizeof(cfg->bind_addr), opt_str(ctx, s, "bind_addr"));
			cfg->max_clients = opt_int(ctx, s, "max_clients", cfg->max_clients);
			cfg->log_level = opt_int(ctx, s, "log", cfg->log_level);
			const char *rtp = opt_str(ctx, s, "rtp");
			cfg->rtp_mode = (rtp && strcmp(rtp, "always") == 0) ? 1 :
				       (rtp && strcmp(rtp, "never") == 0) ? 2 : 0;
			opt_bool(ctx, s, "reorder", &cfg->reorder);
			set_str(cfg->auth_host, sizeof(cfg->auth_host),
				opt_str(ctx, s, "auth_host"));
			opt_bool(ctx, s, "fcc", &cfg->fcc);
			opt_bool(ctx, s, "xcode", &cfg->xcode);
			struct uci_option *fl = uci_lookup_option(ctx, s,
								  "fcc_server");
			if (fl && fl->type == UCI_TYPE_LIST) {
				struct uci_element *le;
				uci_foreach_element(&fl->v.list, le) {
					if (cfg->n_fcc_srv < FCC_SRV_MAX)
						snprintf(cfg->fcc_srv[cfg->n_fcc_srv++],
							 sizeof(cfg->fcc_srv[0]),
							 "%s", le->name);
				}
			}
		} else if (strcmp(s->type, "account") == 0) {
			set_str(cfg->userid, sizeof(cfg->userid), opt_str(ctx, s, "userid"));
			set_str(cfg->sn, sizeof(cfg->sn), opt_str(ctx, s, "sn"));
			set_str(cfg->mac, sizeof(cfg->mac), opt_str(ctx, s, "mac"));
		}
	}
	uci_free_context(ctx);

	if (!cfg->bind[0])
		snprintf(cfg->bind, sizeof(cfg->bind), "0.0.0.0");
	if (cfg->port <= 0 || cfg->port > 65535)
		cfg->port = 4022;
	return 0;
}
