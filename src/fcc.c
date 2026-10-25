/* See fcc.h.  Packet layouts (network order) from the protocol facts
 * in docs/rtp2httpd-reference.md:
 *
 *   FMT=2 request, 40 bytes: RTCP head [V2|FMT2, PT=205, len=9],
 *   sender SSRC 0, media SSRC = group IP; FCI at +12: version(1B)=0 +
 *   reserved(3B), +16 client port, +18 group port, +20 group IP,
 *   +24..39 zero.
 *
 *   FMT=3 response, >=36 bytes: +12 result (0=ok), +13 type,
 *   +14 new signaling port (0=keep), +16 new media port, +20 new
 *   server IP (0=keep), +24 valid seconds, +28 speed bps,
 *   +32 synced bps.
 *
 *   FMT=4 sync notify, >=12 bytes; FMT=5 stop, 16 bytes: +12 stop
 *   flag (0=normal, 1=force), +14 first multicast RTP seq seen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <arpa/inet.h>

#include "fcc.h"
#include "log.h"
#include "util.h"

#define FCC_WAIT_MS    80          /* no FMT=3 by then: give up */
#define FCC_STALL_MS   1000        /* no media for this long: fallback */
#define FCC_REDIRECTS  1           /* FMT=3 redirects we follow */

enum { ST_REQ = 1, ST_ACC = 2 };

static struct {
	int enabled;
	in_addr_t ifaddr;
	struct sockaddr_in srv[FCC_SRV_MAX];
	int nsrv;
} F;

struct fcc_sess {
	int fd;
	in_addr_t group;
	int group_port;
	int state;                     /* ST_REQ / ST_ACC */
	int cli_port;                  /* our declared reception port */
	struct sockaddr_in winner;     /* who answered ok / pushes media */
	int have_winner;
	int redirect_left;
	int64_t deadline_ms;
	int64_t last_media_ms;
};

/* -------- wire helpers -------- */

static void put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

/* in_addr_t is already network order: copy it byte-wise, never
 * endian-swap it through a host integer */
static void put_ip(unsigned char *p, in_addr_t ip)
{
	memcpy(p, &ip, 4);
}

static uint16_t get16(const unsigned char *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

int fcc_build_fmt2(unsigned char *out, in_addr_t group, int group_port,
		   int client_port)
{
	memset(out, 0, 40);
	out[0] = 0x80 | 2;             /* V=2, P=0, FMT=2 */
	out[1] = 205;
	put16(out + 2, 9);             /* length in words minus one */
	put_ip(out + 8, group);        /* media SSRC = group IP */
	/* +12 version 0 + reserved, +16 client port, +18 group port */
	put16(out + 16, (uint16_t)client_port);
	put16(out + 18, (uint16_t)group_port);
	put_ip(out + 20, group);
	return 40;
}

int fcc_build_fmt5(unsigned char *out, in_addr_t group, int force,
		   uint16_t first_mseq)
{
	memset(out, 0, 16);
	out[0] = 0x80 | 5;
	out[1] = 205;
	put16(out + 2, 3);
	put_ip(out + 8, group);
	out[12] = force ? 1 : 0;
	put16(out + 14, first_mseq);
	return 16;
}

/* -------- config -------- */

void fcc_init(const struct iptv_cfg *cfg)
{
	F.enabled = 0;
	F.nsrv = 0;
	F.ifaddr = INADDR_ANY;
	if (!cfg || !cfg->fcc)
		return;

	char addr[48] = "";
	if (cfg->bind_addr[0]) {
		snprintf(addr, sizeof(addr), "%s", cfg->bind_addr);
	} else if (cfg->device[0] &&
		   util_if_addr(cfg->device, addr, sizeof(addr)) != 0) {
		log_msg(LL_WARN, "fcc: no address on '%s', disabled",
			cfg->device);
		return;
	}
	if (addr[0]) {
		F.ifaddr = inet_addr(addr);
		if (F.ifaddr == (in_addr_t)-1)
			F.ifaddr = INADDR_ANY;
	}

	for (int i = 0; i < cfg->n_fcc_srv; i++) {
		char host[48];
		snprintf(host, sizeof(host), "%s", cfg->fcc_srv[i]);
		char *colon = strrchr(host, ':');
		if (!colon)
			continue;
		*colon = '\0';
		int port = atoi(colon + 1);
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)port);
		if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || port < 1) {
			log_msg(LL_WARN, "fcc: bad server '%s'", cfg->fcc_srv[i]);
			continue;
		}
		F.srv[F.nsrv++] = sa;
	}
	F.enabled = 1;
	log_msg(LL_INFO, F.nsrv ?
		"fcc: enabled, %d fallback server(s); per-channel servers "
		"come from the operator channel list" :
		"fcc: enabled, servers resolved per channel from the "
		"operator channel list", F.nsrv);
}

int fcc_enabled(void)
{
	return F.enabled;
}

int fcc_server_count(void)
{
	return F.nsrv;
}

/* -------- session -------- */

static int send_fmt2(struct fcc_sess *s, const struct sockaddr_in *to)
{
	unsigned char req[40];
	int len = fcc_build_fmt2(req, s->group, s->group_port, s->cli_port);
	if (sendto(s->fd, req, (size_t)len, 0,
		   (struct sockaddr *)to, sizeof(*to)) < 0) {
		log_msg(LL_WARN, "fcc: send: %s", strerror(errno));
		return -1;
	}
	return 0;
}

struct fcc_sess *fcc_start(in_addr_t group, int group_port, const char *srv)
{
	if (!F.enabled)
		return NULL;

	/* targets: the channel's operator-designated server, else the
	 * configured fallback list */
	struct sockaddr_in tgt[FCC_SRV_MAX];
	int ntgt = 0;
	const char *via = "uci fallback";
	if (srv && *srv) {
		char host[48];
		snprintf(host, sizeof(host), "%s", srv);
		char *colon = strrchr(host, ':');
		int ok = 0;
		if (colon) {
			*colon = '\0';
			int p = atoi(colon + 1);
			struct sockaddr_in sa;
			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons((unsigned short)p);
			if (inet_pton(AF_INET, host, &sa.sin_addr) == 1 && p >= 1) {
				/* port 8027 is the Huawei FCC family (different
				 * FMT set) - not implemented, plain multicast */
				if (p == 8027) {
					log_msg(LL_INFO,
						"fcc: %s: Huawei server, skipped", srv);
					return NULL;
				}
				tgt[ntgt++] = sa;
				ok = 1;
				via = srv;
			}
		}
		if (!ok) {
			log_msg(LL_WARN, "fcc: bad per-channel server '%s'", srv);
			return NULL;
		}
	} else {
		for (int i = 0; i < F.nsrv; i++)
			tgt[ntgt++] = F.srv[i];
		if (ntgt == 0)
			return NULL;          /* no channel data, no fallback */
	}

	struct fcc_sess *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->group = group;
	s->group_port = group_port;
	s->state = ST_REQ;
	s->redirect_left = FCC_REDIRECTS;

	s->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (s->fd < 0) {
		free(s);
		return NULL;
	}
	fcntl(s->fd, F_SETFL, O_NONBLOCK);

	/* source IP = IPTV interface so replies/media route back */
	struct sockaddr_in ba;
	memset(&ba, 0, sizeof(ba));
	ba.sin_family = AF_INET;
	ba.sin_addr.s_addr = F.ifaddr;
	if (bind(s->fd, (struct sockaddr *)&ba, sizeof(ba)) < 0) {
		ba.sin_addr.s_addr = INADDR_ANY;
		if (bind(s->fd, (struct sockaddr *)&ba, sizeof(ba)) < 0) {
			close(s->fd);
			free(s);
			return NULL;
		}
	}
	struct sockaddr_in actual;
	socklen_t alen = sizeof(actual);
	getsockname(s->fd, (struct sockaddr *)&actual, &alen);
	s->cli_port = ntohs(actual.sin_port);

	for (int i = 0; i < ntgt; i++)
		send_fmt2(s, &tgt[i]);
	s->deadline_ms = util_now_ms() + FCC_WAIT_MS;
	log_msg(LL_INFO, "fcc: requested %s:%d via %s (port %d)",
		inet_ntoa((struct in_addr){ s->group }), group_port,
		via, s->cli_port);
	return s;
}

int fcc_fd(const struct fcc_sess *s)
{
	return s ? s->fd : -1;
}

int fcc_pkt(struct fcc_sess *s, unsigned char *buf, int len,
	    const struct sockaddr_in *from)
{
	/* accelerated media: RTP MP2T from the winning server */
	if (len >= 12 && (buf[0] >> 6) == 2 && (buf[1] & 0x7f) == 33 &&
	    s->state == ST_ACC && s->have_winner &&
	    from->sin_addr.s_addr == s->winner.sin_addr.s_addr) {
		s->last_media_ms = util_now_ms();
		return FCC_PKT_MEDIA;
	}
	if (len < 12 || buf[1] != 205)
		return FCC_PKT_IGNORE;

	int fmt = buf[0] & 0x1f;
	if (fmt == 3 && len >= 36 && s->state == ST_REQ) {
		if (buf[12] != 0) {           /* non-zero: keep multicast */
			log_msg(LL_INFO, "fcc: refused (result %d)", buf[12]);
			return FCC_PKT_IGNORE;
		}
		uint16_t sig_port = get16(buf + 14);
		int new_ip = memcmp(buf + 20, "\x00\x00\x00\x00", 4) != 0;
		if ((sig_port || new_ip) && s->redirect_left > 0) {
			s->redirect_left--;
			struct sockaddr_in to = *from;
			if (new_ip)
				memcpy(&to.sin_addr.s_addr, buf + 20, 4);
			if (sig_port)
				to.sin_port = htons(sig_port);
			send_fmt2(s, &to);
			s->deadline_ms = util_now_ms() + FCC_WAIT_MS;
			log_msg(LL_INFO, "fcc: redirected to %s:%d",
				inet_ntoa(to.sin_addr), ntohs(to.sin_port));
			return FCC_PKT_RESP;
		}
		s->winner = *from;
		s->have_winner = 1;
		s->state = ST_ACC;
		s->last_media_ms = util_now_ms();
		log_msg(LL_INFO, "fcc: accelerating via %s:%d (speed %u bps, %us)",
			inet_ntoa(from->sin_addr), ntohs(from->sin_port),
			get32(buf + 28), get32(buf + 24));
		return FCC_PKT_RESP;
	}
	if (fmt == 4 && s->state == ST_ACC) {
		log_msg(LL_INFO, "fcc: sync notify, back to multicast");
		return FCC_PKT_SYNC;
	}
	return FCC_PKT_IGNORE;
}

int fcc_pending(const struct fcc_sess *s)
{
	return s && s->state == ST_REQ;
}

int64_t fcc_deadline_ms(const struct fcc_sess *s)
{
	return s ? s->deadline_ms : 0;
}

int fcc_stalled(const struct fcc_sess *s)
{
	return s && s->state == ST_ACC &&
	       util_now_ms() - s->last_media_ms >= FCC_STALL_MS;
}

void fcc_finish(struct fcc_sess *s, int force, uint16_t first_mseq)
{
	if (!s)
		return;
	if (s->have_winner) {
		unsigned char stop[16];
		fcc_build_fmt5(stop, s->group, force, first_mseq);
		sendto(s->fd, stop, sizeof(stop), 0,
		       (struct sockaddr *)&s->winner, sizeof(s->winner));
		log_msg(LL_INFO, "fcc: stop sent (%s, first mcast seq %u)",
			force ? "force" : "normal", first_mseq);
	}
	fcc_close(s);
}

void fcc_close(struct fcc_sess *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s);
}
