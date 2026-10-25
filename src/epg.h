#ifndef IPTV_EPG_H
#define IPTV_EPG_H

#include <stdint.h>
#include <stddef.h>

#include "auth.h"

/* 0.3: channel table merge (ottauth channelArray + epg7 category
 * lists), per-channel program guide with cache, now/next passthrough
 * and a background-built whole-grid search index.  Ported from the
 * python reference app/iptv/epg.py. */

#define EPG_CH_MAX   400
#define EPG_NAME_MAX 96

struct epg_channel {
	int no;                     /* mixNo, or 9000+n for leftovers */
	char name[64];
	char pid[40];               /* epg7 short pid, or 32-digit SDP pid */
	char code[64];              /* Umai:CHAN/... */
	char chid[24];              /* epg7 channel ID */
	char igmp[64];              /* igmp://group:port */
	int ts;                     /* timeshift-capable */
	int hd, ac3;
	char timeshift_url[512];
	char fcc[40];               /* operator FCC server "ip:port", "" if none */
	char cats[80];              /* comma separated category names */
};

struct epg_prog {
	char id[24];
	char name[EPG_NAME_MAX];
	int64_t start_ms, end_ms;
};

int  epg_mgr_start(void);          /* background refresher */
int  epg_channel_count(void);
/* snapshot the merged table; returns count copied */
int  epg_channels_copy(struct epg_channel *out, int max);
int  epg_find_channel(const char *pid, struct epg_channel *out);
/* operator-designated FCC server for a live group (r16); 0 if the
 * channel has none or the table is not built yet */
int  epg_fcc_for_group(uint32_t group, int port, char *out, size_t cap);
/* 1.1: the channel on this live group carries AC-3 audio (xcode) */
int  epg_ac3_for_group(uint32_t group, int port);

/* program guide for a channel: cached 15 min, fetched on demand.
 * progs points into module storage valid until the next call - copy
 * if needed.  Returns count, -1 on error/no channel. */
int  epg_programs(const char *pid, const struct epg_prog **progs);

/* now/next for a channel: malloc'd serialized "data" JSON, NULL on
 * failure (caller frees) */
char *epg_now_next(const char *pid);

/* whole-grid search (background index, 30 min TTL); malloc'd JSON
 * array of matches, newest first, capped at 200 items. */
char *epg_search(const char *q);
void epg_search_state(int *building, int64_t *age_s);

/* 0.5 TVOD: epg7 getTvodPlayUrl -> authenticated HTTP HLS playURL
 * (start_s/end_s unix seconds, playbill_id from /api/programs ids).
 * 0 on success with url_out filled. */
int epg_tvod_play_url(const char *pid, const char *playbill_id,
		      int64_t start_s, int64_t end_s, char *url_out,
		      size_t cap);

/* test/debug entry: rebuild the table from an explicit auth state */
int epg_refresh_channels(const struct auth_state *st);

#endif
