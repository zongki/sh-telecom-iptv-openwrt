/* udpxy-compatible multicast relay.
 *
 * - One UDP socket per (group, port): bound to the group address,
 *   IP_ADD_MEMBERSHIP on the configured source device (the IPTV VLAN
 *   interface), refcounted; leave + close when the last client goes.
 * - Per-client byte FIFO (4 MiB); slow clients that overflow the FIFO
 *   are kicked instead of stalling the group thread.
 * - RTP/MP2T handling verified against the live Shanghai Telecom feed
 *   (V=2, PT=33, X=1 with a 4-word proprietary extension, i.e. the
 *   payload actually starts past the 12-byte base header).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mcast.h"
#include "fcc.h"
#include "iptv.h"
#include "log.h"
#include "util.h"

#define CLBUF_SIZE   (4 * 1024 * 1024)
#define MAX_GROUPS   64
#define MAX_CLIENTS  256

/* RTP reorder window (0.1.1): jitter that swaps two RTP packets on the
 * wire reaches clients as a corrupted TS burst.  The group thread holds
 * out-of-order packets briefly and releases them in seq order; in-order
 * streams pass through with zero added latency, and a genuinely lost
 * packet is skipped after RO_HOLD_MS (same visible behavior as 0.1.0).
 */
#define RO_SLOTS   64               /* power of two */
#define RO_HOLD_MS 40
#define RO_MTU     1472             /* max payload worth holding */
/* The accelerated FCC push runs ahead of the local multicast view by
 * seconds (measured ~850 seqs / ~3s on the live feed), and after the
 * FMT=4 merge the relay drops multicast seqs <= last pushed while it
 * catches up.  Anything behind by less than this is that overshoot
 * wait; beyond it the seqs come from a different space - resync. */
#define RO_RESYNC_BEHIND 8192

struct ro_slot {
	int len;                    /* 0 = empty */
	unsigned char data[RO_MTU];
};

struct mgroup {
	struct mgroup *next;
	in_addr_t ip;
	int port;
	int fd;
	int refcnt;
	int stop;
	pthread_t thread;
	pthread_mutex_t lock;      /* protects clients list */
	struct mcast_client *clients;
	/* 0.6 FCC fast channel change (group thread only) */
	struct fcc_sess *fcc;      /* active session, NULL once over */
	int fcc_out_any;           /* pushed >=1 accelerated packet */
	int fcc_have_mseq;         /* saw a multicast RTP seq during FCC */
	uint16_t fcc_first_mseq;   /* first of those (reported in FMT=5) */
	uint16_t fcc_last_seq;     /* last accelerated seq pushed */
	int fcc_floor_on;          /* non-reorder seq filter after FCC */
	uint16_t fcc_floor;
	/* reorder state below, touched only by the group thread */
	uint16_t ro_next;          /* next expected RTP seq */
	int ro_seen;               /* first seq observed */
	int ro_held;               /* filled slots in the window */
	int64_t ro_hold_ms;        /* first-hold timestamp, 0 = nothing held */
	struct ro_slot ro_win[RO_SLOTS];
};

struct mcast_client {
	struct mcast_client *next; /* group list linkage */
	struct mgroup *g;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	unsigned char *buf;
	size_t cap, head, len;     /* ring: head = first byte, len = fill */
	int dead;                  /* 0 live, 1 closed, 2 kicked (overflow) */
	int64_t bytes_out;
};

static struct {
	char device[32];
	in_addr_t ifaddr;
	int rtp_mode;
	int reorder;
	struct mgroup *groups;
	int ngroups;
	int nclients;
	pthread_mutex_t glock;     /* groups list + counters */
} S = { .glock = PTHREAD_MUTEX_INITIALIZER };

/* -------- RTP -------- */

/* Payload offset if the packet looks like RTP carrying MP2T, else 0. */
static int rtp_payload_offset(const unsigned char *p, int len)
{
	if (len < 12 || (p[0] >> 6) != 2 || (p[1] & 0x7f) != 33)
		return 0;
	int off = 12 + 4 * (p[0] & 0x0f);
	if (p[0] & 0x10) {         /* header extension */
		if (len < off + 4)
			return 0;
		off += 4 + 4 * (((int)p[off + 2] << 8) | p[off + 3]);
	}
	if (off + 188 > len)       /* room for at least one TS packet */
		return 0;
	return off;
}

/* -------- group listener -------- */

static void group_push(struct mgroup *g, const unsigned char *data, int len)
{
	pthread_mutex_lock(&g->lock);
	struct mcast_client *c;
	for (c = g->clients; c; c = c->next) {
		pthread_mutex_lock(&c->mu);
		if (!c->dead) {
			if (c->len + (size_t)len > c->cap) {
				c->dead = 2;           /* slow client: kick */
				log_msg(LL_WARN, "relay: slow client kicked (overflow)");
			} else {
				size_t tail = (c->head + c->len) % c->cap;
				size_t first = c->cap - tail;
				if (first > (size_t)len)
					first = len;
				memcpy(c->buf + tail, data, first);
				if ((size_t)len > first)
					memcpy(c->buf, data + first, (size_t)len - first);
				c->len += len;
			}
		}
		pthread_cond_broadcast(&c->cv);
		pthread_mutex_unlock(&c->mu);
	}
	pthread_mutex_unlock(&g->lock);
}

/* -------- RTP reorder (group thread only) -------- */

static void ro_drain(struct mgroup *g)
{
	struct ro_slot *s = &g->ro_win[g->ro_next % RO_SLOTS];
	while (s->len) {
		group_push(g, s->data, s->len);
		s->len = 0;
		g->ro_held--;
		g->ro_next++;
		s = &g->ro_win[g->ro_next % RO_SLOTS];
	}
	if (!g->ro_held)
		g->ro_hold_ms = 0;
}

/* Release everything held in seq order, skipping seqs that never came.
 * Used on hold timeout, oversized gap and oversized packet. */
static void ro_flush(struct mgroup *g)
{
	int guard = 2 * RO_SLOTS;
	while (g->ro_held && guard-- > 0) {
		struct ro_slot *s = &g->ro_win[g->ro_next % RO_SLOTS];
		if (s->len) {
			group_push(g, s->data, s->len);
			s->len = 0;
			g->ro_held--;
		}
		g->ro_next++;
	}
	g->ro_hold_ms = 0;
}

static void ro_input(struct mgroup *g, uint16_t seq,
		     const unsigned char *payload, int len)
{
	if (!g->ro_seen) {
		g->ro_seen = 1;
		g->ro_next = seq + 1;
		group_push(g, payload, len);
		return;
	}
	/* signed seq distance, wrap-aware */
	int16_t d = (int16_t)(seq - g->ro_next);
	if (d == 0) {
		group_push(g, payload, len);
		g->ro_next++;
		ro_drain(g);
		return;
	}
	if (d <= -RO_RESYNC_BEHIND) {
		/* grossly behind: not a late duplicate nor an FCC-merge
		 * overshoot wait - a genuinely divergent seq space.  Only
		 * then resync, or packets drop until the seq wraps. */
		ro_flush(g);
		g->ro_next = seq + 1;
		group_push(g, payload, len);
		return;
	}
	if (d < 0)
		return;   /* late duplicate, or multicast still catching up
			   * with the FCC push: hold, the client has buffer */
	if (d >= RO_SLOTS || len > RO_MTU) {
		/* cannot bridge the gap: ship what is held, resync here */
		ro_flush(g);
		g->ro_next = seq + 1;
		group_push(g, payload, len);
		return;
	}
	struct ro_slot *s = &g->ro_win[seq % RO_SLOTS];
	if (!s->len)
		g->ro_held++;
	memcpy(s->data, payload, (size_t)len);
	s->len = len;
	if (!g->ro_hold_ms)
		g->ro_hold_ms = util_now_ms();
}

static void ro_tick(struct mgroup *g)
{
	if (g->ro_hold_ms && util_now_ms() - g->ro_hold_ms >= RO_HOLD_MS)
		ro_flush(g);
}

/* -------- FCC interworking (0.6, group thread only) -------- */

static void ro_reset(struct mgroup *g, int seen, uint16_t next)
{
	for (int i = 0; i < RO_SLOTS; i++)
		g->ro_win[i].len = 0;
	g->ro_seen = seen;
	g->ro_next = next;
	g->ro_held = 0;
	g->ro_hold_ms = 0;
}

/* FCC session over (FMT=4 sync or stall): stop the signaling and
 * return to multicast, dropping seqs the accelerated push already
 * delivered so the merge is seamless. */
static void end_fcc(struct mgroup *g, int force)
{
	uint16_t first = g->fcc_have_mseq ? g->fcc_first_mseq : 0;
	fcc_finish(g->fcc, force, first);
	g->fcc = NULL;
	if (!g->fcc_out_any)
		return;                  /* nothing pushed: clean restart */
	if (force)
		/* forced stop mid-push: nothing guarantees the push and the
		 * local multicast view share a seq alignment here (only the
		 * FMT=4 sync point does) - let the reorder state restart on
		 * the next packet instead of filtering against its seq */
		return;
	/* seed the seq machinery so packets <= last pushed seq drop */
	if (S.reorder)
		ro_reset(g, 1, (uint16_t)(g->fcc_last_seq + 1));
	else {
		g->fcc_floor = g->fcc_last_seq;
		g->fcc_floor_on = 1;
	}
}

static void fcc_input(struct mgroup *g, unsigned char *pkt, int n,
		      const struct sockaddr_in *from)
{
	int rc = fcc_pkt(g->fcc, pkt, n, from);
	if (rc == FCC_PKT_SYNC) {
		end_fcc(g, 0);
		return;
	}
	if (rc != FCC_PKT_MEDIA)
		return;
	int off = (S.rtp_mode == 2) ? 0 : rtp_payload_offset(pkt, n);
	if (S.rtp_mode == 1 && off == 0 && n > 12)
		off = 12;
	if (off > 0) {
		g->fcc_last_seq = ((uint16_t)pkt[2] << 8) | pkt[3];
		g->fcc_out_any = 1;
	}
	group_push(g, pkt + off, n - off);
}

/* one inbound multicast datagram: during an FCC session only seqs are
 * tracked (the accelerated push is the output); right after a session
 * a seq floor drops already-sent packets; otherwise normal handling */
static void group_input(struct mgroup *g, unsigned char *pkt, int n)
{
	int off = (S.rtp_mode == 2) ? 0 : rtp_payload_offset(pkt, n);
	if (S.rtp_mode == 1 && off == 0 && n > 12)
		off = 12;            /* forced strip */

	if (g->fcc) {
		if (off > 0) {
			uint16_t seq = ((uint16_t)pkt[2] << 8) | pkt[3];
			if (!g->fcc_have_mseq) {
				g->fcc_have_mseq = 1;
				g->fcc_first_mseq = seq;
			}
		}
		return;                  /* held while FCC runs */
	}
	if (off > 0 && g->fcc_floor_on) {
		uint16_t seq = ((uint16_t)pkt[2] << 8) | pkt[3];
		int16_t d = (int16_t)(seq - g->fcc_floor);
		if (d <= 0 && d > -RO_RESYNC_BEHIND)
			return;          /* the push already delivered it, or the
					   * multicast is still catching up */
		g->fcc_floor_on = 0;     /* first new (or divergent) seq */
	}
	if (off > 0 && S.reorder) {
		uint16_t seq = ((uint16_t)pkt[2] << 8) | pkt[3];
		ro_input(g, seq, pkt + off, n - off);
		ro_tick(g);
	} else {
		group_push(g, pkt + off, n - off);
	}
}

static void *group_thread(void *arg)
{
	struct mgroup *g = arg;
	unsigned char pkt[65536];
	struct sockaddr_in from;
	while (!g->stop) {
		if (!g->fcc) {
			int n = recv(g->fd, pkt, sizeof(pkt), 0);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					ro_tick(g);  /* recv timeout: flush held packets */
					continue;
				}
				if (!g->stop)
					log_msg(LL_WARN, "relay: recv: %s", strerror(errno));
				break;
			}
			group_input(g, pkt, n);
			continue;
		}

		/* FCC session: multiplex multicast + the unicast push, never
		 * block on one source alone */
		struct pollfd pfd[2] = {
			{ .fd = g->fd, .events = POLLIN },
			{ .fd = fcc_fd(g->fcc), .events = POLLIN },
		};
		poll(pfd, 2, 100);
		if (pfd[0].revents & POLLIN) {
			int n = recv(g->fd, pkt, sizeof(pkt), 0);
			if (n > 0)
				group_input(g, pkt, n);
		}
		if (g->fcc && pfd[1].fd >= 0 && (pfd[1].revents & POLLIN)) {
			socklen_t flen = sizeof(from);
			int n = recvfrom(pfd[1].fd, pkt, sizeof(pkt), 0,
					 (struct sockaddr *)&from, &flen);
			if (n > 0)
				fcc_input(g, pkt, n, &from);
		}
		if (g->fcc && fcc_pending(g->fcc) &&
		    util_now_ms() >= fcc_deadline_ms(g->fcc)) {
			/* no FMT=3 within the 80ms budget: plain multicast */
			log_msg(LL_INFO, "relay: fcc no response, multicast only");
			fcc_close(g->fcc);
			g->fcc = NULL;
		} else if (g->fcc && fcc_stalled(g->fcc)) {
			log_msg(LL_WARN, "relay: fcc push stalled, fallback");
			end_fcc(g, 1);
		}
		ro_tick(g);
	}
	/* a session still alive at teardown must be torn down too */
	if (g->fcc) {
		uint16_t first = g->fcc_have_mseq ? g->fcc_first_mseq : 0;
		fcc_finish(g->fcc, 1, first);
		g->fcc = NULL;
	}
	return NULL;
}

static void group_teardown(struct mgroup *g)
{
	/* caller holds S.glock and g is already unlinked */
	struct ip_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = g->ip;
	mreq.imr_interface.s_addr = S.ifaddr;
	setsockopt(g->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
	g->stop = 1;
	shutdown(g->fd, 0);
	close(g->fd);
	pthread_join(g->thread, NULL);
	pthread_mutex_destroy(&g->lock);
	log_msg(LL_INFO, "relay: left %s:%d",
		inet_ntoa((struct in_addr){ g->ip }), g->port);
	free(g);
}

/* -------- public API -------- */

struct mcast_client *mcast_relay_open(in_addr_t group, int port,
			      const char *fcc_srv, int fcc_en)
{
	if (!mcast_ready() || port < 1 || port > 65535 ||
	    !IN_MULTICAST(ntohl(group)))
		return NULL;

	struct mcast_client *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->buf = malloc(CLBUF_SIZE);
	if (!c->buf) {
		free(c);
		return NULL;
	}
	c->cap = CLBUF_SIZE;
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv, NULL);

	pthread_mutex_lock(&S.glock);
	struct mgroup *g;
	for (g = S.groups; g; g = g->next)
		if (g->ip == group && g->port == port)
			break;
	if (!g) {
		if (S.ngroups >= MAX_GROUPS || S.nclients >= MAX_CLIENTS)
			goto fail;
		g = calloc(1, sizeof(*g));
		if (!g)
			goto fail;
		g->ip = group;
		g->port = port;
		pthread_mutex_init(&g->lock, NULL);
		g->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (g->fd < 0)
			goto fail_free;

		int one = 1;
		setsockopt(g->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		int rcvbuf = 4 * 1024 * 1024;
		setsockopt(g->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

		/* Wake the group thread periodically so held reorder packets
		 * are flushed even when the source stalls mid-gap. */
		struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
		setsockopt(g->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		struct sockaddr_in bindaddr;
		memset(&bindaddr, 0, sizeof(bindaddr));
		bindaddr.sin_family = AF_INET;
		bindaddr.sin_port = htons((unsigned short)port);
		bindaddr.sin_addr.s_addr = group;
		if (bind(g->fd, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
			log_msg(LL_ERR, "relay: bind(%s:%d): %s",
				inet_ntoa((struct in_addr){ group }), port,
				strerror(errno));
			goto fail_fd;
		}

		struct ip_mreq mreq;
		memset(&mreq, 0, sizeof(mreq));
		mreq.imr_multiaddr.s_addr = group;
		mreq.imr_interface.s_addr = S.ifaddr;
		if (setsockopt(g->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
			       &mreq, sizeof(mreq)) < 0) {
			log_msg(LL_ERR, "relay: join(%s): %s",
				inet_ntoa((struct in_addr){ group }),
				strerror(errno));
			goto fail_fd;
		}
		/* IGMP reports must egress the IPTV interface whatever the
		 * routing table would prefer. */
		setsockopt(g->fd, IPPROTO_IP, IP_MULTICAST_IF,
			   &S.ifaddr, sizeof(S.ifaddr));

		/* channel change: ask for an accelerated unicast push.
		 * fcc_srv is the channel's operator-designated FCC server
		 * (resolved by the caller); NULL = fallback list / none.
		 * fcc_en=0 (?nofcc=1): skip the push entirely - the push
		 * stream occasionally drops frames (measured "Reference
		 * N >= 2" decode errors inside captures of the push), so
		 * the degrade path after a playback error goes plain. */
		if (fcc_en && fcc_enabled())
			g->fcc = fcc_start(group, port, fcc_srv);

		if (pthread_create(&g->thread, NULL, group_thread, g) != 0)
			goto fail_join;

		g->refcnt = 0;
		g->next = S.groups;
		S.groups = g;
		S.ngroups++;
		log_msg(LL_INFO, "relay: joined %s:%d",
			inet_ntoa((struct in_addr){ group }), port);
	} else if (S.nclients >= MAX_CLIENTS) {
		goto fail;
	}
	g->refcnt++;
	S.nclients++;
	pthread_mutex_unlock(&S.glock);

	/* attach to group client list */
	c->g = g;
	pthread_mutex_lock(&g->lock);
	c->next = g->clients;
	g->clients = c;
	pthread_mutex_unlock(&g->lock);
	return c;

fail_join:
	{
		struct ip_mreq m2;
		memset(&m2, 0, sizeof(m2));
		m2.imr_multiaddr.s_addr = group;
		m2.imr_interface.s_addr = S.ifaddr;
		setsockopt(g->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m2, sizeof(m2));
	}
	fcc_close(g->fcc);
fail_fd:
	close(g->fd);
fail_free:
	pthread_mutex_destroy(&g->lock);
	free(g);
fail:
	pthread_mutex_unlock(&S.glock);
	pthread_cond_destroy(&c->cv);
	pthread_mutex_destroy(&c->mu);
	free(c->buf);
	free(c);
	return NULL;
}

int mcast_relay_read(struct mcast_client *c, void *out, size_t max)
{
	pthread_mutex_lock(&c->mu);
	while (c->len == 0 && !c->dead) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 2;
		pthread_cond_timedwait(&c->cv, &c->mu, &ts);
	}
	if (c->len == 0) {
		int dead = c->dead;
		pthread_mutex_unlock(&c->mu);
		return dead ? -1 : 0;
	}
	size_t n = c->len < max ? c->len : max;
	size_t first = c->cap - c->head;
	if (first > n)
		first = n;
	memcpy(out, c->buf + c->head, first);
	if (n > first)
		memcpy((unsigned char *)out + first, c->buf, n - first);
	c->head = (c->head + n) % c->cap;
	c->len -= n;
	c->bytes_out += n;
	pthread_mutex_unlock(&c->mu);
	return (int)n;
}

/* Mark the client dead so a blocked mcast_relay_read (e.g. the xcode
 * pump thread) returns promptly; the struct is still freed by
 * mcast_relay_close. */
void mcast_relay_halt(struct mcast_client *c)
{
	if (!c)
		return;
	pthread_mutex_lock(&c->mu);
	c->dead = 1;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);
}

void mcast_relay_close(struct mcast_client *c)
{
	if (!c)
		return;
	struct mgroup *g = c->g;

	pthread_mutex_lock(&c->mu);
	c->dead = 1;
	pthread_cond_broadcast(&c->cv);
	pthread_mutex_unlock(&c->mu);

	if (g) {
		pthread_mutex_lock(&g->lock);
		struct mcast_client **pp = &g->clients;
		while (*pp && *pp != c)
			pp = &(*pp)->next;
		if (*pp)
			*pp = c->next;
		pthread_mutex_unlock(&g->lock);
	}

	pthread_cond_destroy(&c->cv);
	pthread_mutex_destroy(&c->mu);
	free(c->buf);
	free(c);

	if (!g)
		return;

	pthread_mutex_lock(&S.glock);
	S.nclients--;
	int last = (--g->refcnt == 0);
	if (last) {
		struct mgroup **pp = &S.groups;
		while (*pp && *pp != g)
			pp = &(*pp)->next;
		if (*pp)
			*pp = g->next;
		S.ngroups--;
	}
	pthread_mutex_unlock(&S.glock);
	if (last)
		group_teardown(g);
}

int mcast_group_count(void)
{
	return S.ngroups;
}

int mcast_client_count(void)
{
	return S.nclients;
}

int mcast_ready(void)
{
	return S.ifaddr != 0;
}

int mcast_init(const char *device, const char *bind_addr, int rtp_mode,
	       int reorder)
{
	S.rtp_mode = rtp_mode;
	S.reorder = reorder;
	if (!device || !*device) {
		log_msg(LL_INFO, "relay: no device configured, relay disabled");
		return 0;
	}
	snprintf(S.device, sizeof(S.device), "%s", device);
	char addr[48];
	if (bind_addr && *bind_addr)
		snprintf(addr, sizeof(addr), "%s", bind_addr);
	else if (util_if_addr(device, addr, sizeof(addr)) != 0) {
		log_msg(LL_ERR, "relay: no IPv4 on '%s'", device);
		return -1;
	}
	S.ifaddr = inet_addr(addr);
	if (S.ifaddr == 0 || S.ifaddr == (in_addr_t)-1) {
		log_msg(LL_ERR, "relay: bad source address '%s'", addr);
		return -1;
	}
	log_msg(LL_INFO, "relay: source %s (%s)", S.device, addr);
	return 0;
}
