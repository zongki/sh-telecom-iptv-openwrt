#ifndef IPTV_MCAST_H
#define IPTV_MCAST_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/* Multicast relay core: per (group,port) listener with refcounted
 * IGMP join/leave and fan-out to HTTP clients. */

struct mcast_client;

int  mcast_init(const char *device, const char *bind_addr, int rtp_mode,
		int reorder);
int  mcast_ready(void);

/* Open a streaming slot on a group (creates+joins the listener on the
 * first reference).  fcc_en=0 skips the accelerated push for this
 * group (the ?nofcc=1 degrade path: the carrier's push stream
 * occasionally drops frames, plain multicast does not).
 * NULL on failure (limits reached, join error...). */
struct mcast_client *mcast_relay_open(in_addr_t group, int port,
			      const char *fcc_srv, int fcc_en);

/* Blocking read for the HTTP layer: >0 bytes copied, 0 orderly end,
 * -1 error / kicked. */
int  mcast_relay_read(struct mcast_client *c, void *buf, size_t max);

/* Detach; leaves the IGMP group when the last client is gone. */
void mcast_relay_close(struct mcast_client *c);

/* 1.1: mark dead without freeing, so a concurrently blocked
 * mcast_relay_read (the xcode pump) returns promptly;
 * mcast_relay_close still owns the free. */
void mcast_relay_halt(struct mcast_client *c);

/* /status counters */
int  mcast_group_count(void);
int  mcast_client_count(void);

#endif
