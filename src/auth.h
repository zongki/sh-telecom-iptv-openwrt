#ifndef IPTV_AUTH_H
#define IPTV_AUTH_H

#include <stddef.h>
#include <stdint.h>

#include "iptv.h"

/* Shanghai Telecom IPTV auth chain (0.2), ported 1:1 from the python
 * reference (app/iptv/auth.py):
 *
 *   pre4kLogAuth -> r4kLogAuth(encrytoken) -> ottauth(authenticator,
 *   usertoken) -> EPG index.jsp -> loadbalance -> funcportalauth ->
 *   jsSetConfig(SessionID/framecode/IpPort) -> portal.jsp ->
 *   AuthByAjax(bimsUserToken)
 *
 * The EPG session lives ~4h; the manager re-runs the chain at 3h. */

#define AUTH_RAW_CH_MAX 400

/* channelArray entries parsed out of the ottauth response (python
 * parse_channels); igmp/timeshift feed the 0.3 epg merge. */
struct auth_raw_channel {
	char pid[40];            /* numeric pid from the SDP path (32 digits live) */
	char igmp[64];           /* igmp://group:port */
	int ts;                  /* TimeShift == "1" */
	char timeshift_url[512]; /* rtsp://...?AuthInfo=<~300-char urlencoded blob> */
	char fcc[40];            /* operator FCC server "ip:port" (ChannelFCCIP/Port), "" if none */
};

struct auth_state {
	int ok;                       /* last chain attempt succeeded */
	int64_t auth_time;            /* epoch seconds of that success */
	char usertoken[128];
	char epggroup[64];
	char session_id[128];
	char epg_host_url[256];       /* http://IpPort/iptvepg/framecode */
	char bims_user_token[256];
	char err[160];                /* last failure reason */
	struct auth_raw_channel raw[AUTH_RAW_CH_MAX];
	int nraw;
};

/* authenticator = AES-128-ECB(MD5("123456"), compact JSON) -> hex.
 * randon is the 8-digit random string (injected for tests). */
int auth_build_authenticator(const char *userid, const char *sn,
			     const char *mac, const char *stb_ip,
			     const char *encrytoken, const char *randon,
			     char *out_hex, size_t cap);

/* Run the whole chain once (blocking, ~7 round trips). */
int auth_run_full(const struct iptv_cfg *cfg, const char *stb_ip,
		  struct auth_state *st);

/* Background session manager (started when account+auth_host set). */
int  auth_mgr_start(const struct iptv_cfg *cfg);  /* 0 if thread started */
int  auth_mgr_enabled(void);
void auth_mgr_snapshot(struct auth_state *out);   /* locked copy */
/* Synchronous re-auth (401/454 recovery path); refreshes the manager
 * state and optionally copies it out.  0 on success. */
int  auth_mgr_force_refresh(struct auth_state *out);

#endif
