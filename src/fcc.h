#ifndef IPTV_FCC_H
#define IPTV_FCC_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "iptv.h"

/* 0.6: Telecom FCC fast channel change, independently implemented
 * from the protocol facts collected in docs/rtp2httpd-reference.md.
 *
 * On group join the relay sends an RTCP FMT=2 request (PT=205, media
 * SSRC = group IP) to every configured FCC server, asking for an
 * accelerated unicast copy of the channel pushed back to our socket.
 * The first FMT=3 with result 0 wins (it may redirect us once to a
 * different server).  With no usable response within 80 ms the group
 * falls back to plain multicast - FCC is best-effort, never fatal.
 * The push ends on an FMT=4 sync notify (normal; caller switches back
 * to multicast filtered by RTP seq) or a 1s media stall (forced).
 * Either way we answer with FMT=5. */

enum {
	FCC_PKT_IGNORE = 0,      /* stray datagram, keep waiting */
	FCC_PKT_MEDIA  = 1,      /* accelerated RTP: caller relays the datagram */
	FCC_PKT_RESP   = 2,      /* FMT=3 handled internally (ok or redirect) */
	FCC_PKT_SYNC   = 3,      /* FMT=4: push over, switch back to multicast */
};

struct fcc_sess;

/* snapshot config + source address (call once at startup) */
void fcc_init(const struct iptv_cfg *cfg);
int  fcc_enabled(void);
int  fcc_server_count(void);

/* all of the session API below runs on the group thread; srv is the
 * operator-designated "ip:port" for this channel (ottauth channelArray,
 * may be NULL to use the configured fallback list) */
struct fcc_sess *fcc_start(in_addr_t group, int group_port, const char *srv);
int  fcc_fd(const struct fcc_sess *s);            /* -1 = none */

/* feed one datagram received on the FCC socket; see FCC_PKT_* */
int  fcc_pkt(struct fcc_sess *s, unsigned char *buf, int len,
	     const struct sockaddr_in *from);

int     fcc_pending(const struct fcc_sess *s);    /* FMT=3 still awaited */
int64_t fcc_deadline_ms(const struct fcc_sess *s);/* give up point */
int     fcc_stalled(const struct fcc_sess *s);    /* >=1s without media */

/* FMT=5 (force=1 abort) + free / just free; both NULL-safe */
void fcc_finish(struct fcc_sess *s, int force, uint16_t first_mseq);
void fcc_close(struct fcc_sess *s);

/* test/debug entries */
int fcc_build_fmt2(unsigned char *out, in_addr_t group, int group_port,
		   int client_port);
int fcc_build_fmt5(unsigned char *out, in_addr_t group, int force,
		   uint16_t first_mseq);

#endif
