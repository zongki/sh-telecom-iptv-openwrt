/* iptv daemon - common definitions
 *
 * Compatibility target: OpenWrt 19.07 (gcc 7 / musl 1.1) .. 25.12
 * (gcc 14 / musl 1.2).  Stick to C99; use explicit int64_t for epoch
 * values so a 32-bit time_t (musl 1.1 on 32-bit targets) never leaks
 * into on-wire or on-disk formats.
 */
#ifndef IPTV_H
#define IPTV_H

#include <stdint.h>
#include <stddef.h>

/* LL_* to avoid colliding with the syslog.h LOG_* macros */
enum { LL_ERR = 0, LL_WARN = 1, LL_INFO = 2, LL_DEBUG = 3 };

#define FCC_SRV_MAX 8           /* 0.6: FCC server list capacity */

struct iptv_cfg {
	int disabled;
	int port;
	char bind[64];
	char device[32];      /* multicast source device, "" = relay off */
	char bind_addr[48];   /* explicit source IPv4, "" = auto from device */
	int max_clients;
	int rtp_mode;          /* 0=auto 1=always 2=never */
	int reorder;           /* RTP seq reorder window (0.1.1) */
	int log_level;
	char auth_host[72];    /* EPG auth server host:port (0.2) */
	/* account section - reserved for auth/EPG modules */
	char userid[64];
	char sn[64];
	char mac[32];
	/* 0.6 FCC fast channel change */
	int fcc;                              /* enable */
	char fcc_srv[FCC_SRV_MAX][48];        /* "ip:port" list */
	int n_fcc_srv;
	/* 1.1 AC-3 -> AAC transcode for the 4K channels; degrades to the
	 * raw stream when ffmpeg is not installed */
	int xcode;
};

#endif /* IPTV_H */
