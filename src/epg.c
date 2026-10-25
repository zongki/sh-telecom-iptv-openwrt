/* See epg.h.  All epg7 traffic goes through one AJAX endpoint
 * (epg7getChannelByAjax.jsp) with the EPG session cookie; the merge
 * rules and magic tables mirror epg.py exactly (they encode
 * reverse-engineered carrier specifics - see the comments there). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "epg.h"
#include "auth.h"
#include "httpc.h"
#include "json.h"
#include "log.h"
#include "util.h"

#define PROG_CACHE_TTL   900        /* 15 min, whole-grid guide fetch */
#define PROG_RANGE_BACK  (7 * 86400)
#define PROG_RANGE_FWD   86400
#define SEARCH_TTL       1800       /* 30 min */
#define SEARCH_WORKERS   8          /* index build concurrency (python
                                     * SEARCH_WORKERS, be gentle on the
                                     * carrier EPG) */
#define PROG_MAX         2000       /* server-side limit in the form */

/* ottauth-only channels named by hand (python PID_NAMES) */
static const struct { const char *pid, *name; } PID_NAMES[] = {
	{ "6000130", "北京卫视4K" }, { "6000133", "广东卫视4K" },
	{ "6000134", "深圳卫视4K" }, { "6000138", "东方卫视4K" },
	{ "6000139", "浙江卫视4K" }, { "6000140", "湖南卫视4K" },
	{ "6000141", "江苏卫视4K" }, { "6000142", "四川卫视4K" },
	{ "6000143", "山东卫视4K" },
};

/* AC-3 audio channels (python AC3_PIDS), kept for the 1.0 frontend */
static const char *const AC3_PIDS[] = {
	"6000120", "6000130", "6000133", "6000134", "6000138",
	"6000139", "6000140", "6000141", "6000142", "6000143",
};

/* (category, epg7 cateID); NULL cateID = name-based (4K) */
static const struct { const char *name, *cate; } CAT_CATES[] = {
	{ "央视", "000405" }, { "卫视", "000409" }, { "本地", "000408" },
	{ "4K", NULL }, { "高清", "000404" }, { "标清", "000403" },
	{ "少儿", "00040B" }, { "收费", "000401" }, { "购物", "000400" },
};

/* verified multicast channel absent from both tables (python) */
static const struct epg_channel STATIC_CHANNELS[] = {
	{ 9999, "CCTV-16奥林匹克4K", "cctv16oly4k", "", "",
	  "igmp://233.18.204.114:5140", 0, 1, 0, "", "", "" },
};

#define SEARCH_ITEM_MAX 60000

/* -------- module state -------- */

struct prog_ent {
	char pid[40];
	int64_t ts;                    /* fetch epoch */
	struct epg_prog *progs;
	int n;
};

struct search_item {
	int no;                        /* channel no */
	char chname[64];
	char pid[40];
	int64_t start_ms, end_ms;
	char name[EPG_NAME_MAX];
	char id[24];
};

static struct {
	pthread_mutex_t lock;          /* channels + prog cache + index */
	struct epg_channel chans[EPG_CH_MAX];
	int nchans;
	struct prog_ent cache[EPG_CH_MAX];
	int ncache;
	struct search_item *items;
	int nitems;
	int64_t search_ts;
	int search_done;
	volatile int search_building;
	int64_t last_auth_time;
	pthread_t thread;
	int started;
} E = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* -------- pid helpers (python _pid_from_code/_short_pid/_pid_match) -------- */

/* Umai:CHAN/1346060@BESTV.SMG.SMG -> 1346060 */
static int pid_from_code(const char *code, char *out, size_t cap)
{
	const char *p = strstr(code, "CHAN/");
	if (!p)
		return -1;
	p += 5;
	size_t n = 0;
	while (p[n] && p[n] != '@' && n + 1 < cap) {
		out[n] = p[n];
		n++;
	}
	out[n] = '\0';
	return n ? 0 : -1;
}

/* SDP long pid: last 8 digits zero-padded channel number */
static void short_pid(const char *long_pid, char *out, size_t cap)
{
	size_t l = strlen(long_pid);
	const char *tail = l > 8 ? long_pid + l - 8 : long_pid;
	long v = strtol(tail, NULL, 10);
	if (v > 0)
		snprintf(out, cap, "%ld", v);
	else
		snprintf(out, cap, "%s", long_pid);
}

/* long pid = prefix + zero-padded number: must end with num and the
 * digit right before it (if any) must be the padding zero */
static int pid_match(const char *long_pid, const char *num)
{
	if (!num || !*num)
		return 0;
	size_t ll = strlen(long_pid), nl = strlen(num);
	if (ll < nl || strcmp(long_pid + ll - nl, num) != 0)
		return 0;
	return ll == nl || long_pid[ll - nl - 1] == '0';
}

/* -------- epg7 AJAX -------- */

/* POST one action; returns parsed JSON (caller j_free), NULL on error */
static struct jval *epg7_ajax(const struct auth_state *st,
			      const char *const *form)
{
	if (!st->ok || !st->epg_host_url[0])
		return NULL;
	char url[512], cookie[512], referer[512];
	snprintf(url, sizeof(url), "%s/function/ajax/epg7getChannelByAjax.jsp",
		 st->epg_host_url);
	snprintf(cookie, sizeof(cookie), "JSESSIONID=%s", st->session_id);
	if (st->bims_user_token[0])
		snprintf(cookie + strlen(cookie),
			 sizeof(cookie) - strlen(cookie),
			 "; bims_user_token=%s", st->bims_user_token);
	snprintf(referer, sizeof(referer), "%s/portal.jsp", st->epg_host_url);

	struct httpc *c = httpc_new();
	if (!c)
		return NULL;
	struct httpc_resp resp;
	struct jval *jv = NULL;
	if (httpc_request(c, url, "POST", form, referer, NULL, cookie,
			  15000, &resp) == 0 && resp.body) {
		jv = json_parse(resp.body);
		if (!jv)
			log_msg(LL_WARN, "epg: bad JSON from epg7 (%.60s)",
				resp.body);
	}
	httpc_resp_free(&resp);
	httpc_free(c);
	return jv;
}

/* fetch a channel list for one cateID (caller j_free) */
static struct jval *epg7_channel_list(const struct auth_state *st,
				      const char *cate_id)
{
	char cate[32];
	snprintf(cate, sizeof(cate), "%s", cate_id);
	const char *form[] = { "action", "getChannelList", "cateID", cate, NULL };
	return epg7_ajax(st, form);
}

/* -------- channel merge (python refresh_channels) -------- */

#define CAT_LIST_MAX 1024

/* one epg7 category list -> short-pid array (caller frees); -1 on
 * fetch failure (that category then labels nothing, as before) */
static int cat_list_fetch(const struct auth_state *st, const char *cate_id,
			  char (*out)[24])
{
	struct jval *list = epg7_channel_list(st, cate_id);
	if (!list)
		return -1;
	int n = 0;
	struct jval *data = j_get(list, "data");
	if (data && data->t == J_ARR)
		for (int d = 0; d < data->n && n < CAT_LIST_MAX; d++) {
			char num[24];
			if (pid_from_code(j_str(j_get(data->items[d], "code"), ""),
					  num, sizeof(num)) == 0)
				snprintf(out[n++], 24, "%s", num);
		}
	j_free(list);
	return n;
}

/* build merged table from st into out (unsorted) */
static int merge_channels(const struct auth_state *st,
			  struct epg_channel *out, int *outn)
{
	int n = 0;
	int matched[AUTH_RAW_CH_MAX] = { 0 };

	struct jval *epg7 = epg7_channel_list(st, "000406");
	if (epg7) {
		struct jval *data = j_get(epg7, "data");
		if (data && data->t == J_ARR) {
			for (int i = 0; i < data->n && n < EPG_CH_MAX; i++) {
				struct jval *ch = data->items[i];
				if (!ch || ch->t != J_OBJ)
					continue;
				char num[24];
				if (pid_from_code(j_str(j_get(ch, "code"), ""),
						  num, sizeof(num)) != 0)
					continue;
				const struct auth_raw_channel *base = NULL;
				for (int k = 0; k < st->nraw; k++) {
					if (matched[k])
						continue;
					if (pid_match(st->raw[k].pid, num)) {
						base = &st->raw[k];
						matched[k] = 1;
						break;
					}
				}				if (!base)
					continue;
				struct epg_channel *m = &out[n++];
				memset(m, 0, sizeof(*m));
				m->no = atoi(j_str(j_get(ch, "mixNo"), "0"));
				snprintf(m->name, sizeof(m->name), "%s",
					 j_str(j_get(ch, "name"), ""));
				snprintf(m->pid, sizeof(m->pid), "%s", num);
				snprintf(m->code, sizeof(m->code), "%s",
					 j_str(j_get(ch, "code"), ""));
				snprintf(m->chid, sizeof(m->chid), "%s",
					 j_str(j_get(ch, "ID"), ""));
				snprintf(m->igmp, sizeof(m->igmp), "%s", base->igmp);
				m->ts = base->ts &&
					strcmp(j_str(j_get(ch, "isTs"), ""), "1") == 0;
				const char *nm = m->name;
				m->hd = strstr(nm, "HD") != NULL || strstr(nm, "4K") != NULL;
				snprintf(m->timeshift_url, sizeof(m->timeshift_url),
					 "%s", base->timeshift_url);
				snprintf(m->fcc, sizeof(m->fcc), "%s", base->fcc);
			}
		}
		j_free(epg7);
	}

	/* ottauth-only channels appended with synthesized numbers */
	int extra = 0;
	for (int k = 0; k < st->nraw && n < EPG_CH_MAX; k++) {
		if (matched[k])
			continue;
		struct epg_channel *m = &out[n++];
		memset(m, 0, sizeof(*m));
		extra++;
		m->no = 9000 + extra;
		char sp[24];
		short_pid(st->raw[k].pid, sp, sizeof(sp));
		const char *named = NULL;
		for (size_t t = 0; t < sizeof(PID_NAMES) / sizeof(PID_NAMES[0]); t++)
			if (strcmp(PID_NAMES[t].pid, sp) == 0)
				named = PID_NAMES[t].name;
		if (named)
			snprintf(m->name, sizeof(m->name), "%s", named);
		else {
			size_t pl = strlen(st->raw[k].pid);
			snprintf(m->name, sizeof(m->name), "频道%s",
				 pl >= 4 ? st->raw[k].pid + pl - 4 : st->raw[k].pid);
		}
		snprintf(m->pid, sizeof(m->pid), "%s", st->raw[k].pid);
		snprintf(m->igmp, sizeof(m->igmp), "%s", st->raw[k].igmp);
		m->ts = st->raw[k].ts;
		m->hd = strstr(m->name, "4K") != NULL;
		snprintf(m->timeshift_url, sizeof(m->timeshift_url),
			 "%s", st->raw[k].timeshift_url);
		snprintf(m->fcc, sizeof(m->fcc), "%s", st->raw[k].fcc);
	}

	/* hand-verified static channels */
	for (size_t s = 0; s < sizeof(STATIC_CHANNELS) / sizeof(STATIC_CHANNELS[0]);
	     s++) {
		if (n >= EPG_CH_MAX)
			break;
		int have = 0;
		for (int i = 0; i < n; i++)
			if (strcmp(out[i].pid, STATIC_CHANNELS[s].pid) == 0)
				have = 1;
		if (!have)
			out[n++] = STATIC_CHANNELS[s];
	}

	/* categories: one fetch per epg7 cateID, then match locally
	 * (python _apply_categories; the per-channel-per-category form
	 * issued ~N*8 sequential requests and took minutes to build) */
	size_t ncats = sizeof(CAT_CATES) / sizeof(CAT_CATES[0]);
	struct cat_list {
		char (*num)[24];
		int n;
	} *cl = calloc(ncats, sizeof(*cl));
	if (cl)
		for (size_t ci = 0; ci < ncats; ci++) {
			if (!CAT_CATES[ci].cate)
				continue;          /* name-based (4K) */
			cl[ci].num = malloc(CAT_LIST_MAX * sizeof(*cl[ci].num));
			if (!cl[ci].num)
				continue;
			cl[ci].n = cat_list_fetch(st, CAT_CATES[ci].cate,
						  cl[ci].num);
		}
	for (int i = 0; i < n; i++) {
		struct epg_channel *m = &out[i];
		m->cats[0] = '\0';
		size_t off = 0;
		for (size_t ci = 0; ci < ncats; ci++) {
			int hit = 0;
			if (!CAT_CATES[ci].cate) {
				hit = strstr(m->name, "4K") != NULL;
			} else if (cl && cl[ci].num && cl[ci].n >= 0) {
				for (int d = 0; d < cl[ci].n && !hit; d++)
					if (pid_match(m->pid, cl[ci].num[d]))
						hit = 1;
			}
			if (hit && off + strlen(CAT_CATES[ci].name) + 2 < sizeof(m->cats)) {
				if (off)
					m->cats[off++] = ',';
				strcpy(m->cats + off, CAT_CATES[ci].name);
				off += strlen(CAT_CATES[ci].name);
			}
		}
	}
	if (cl) {
		for (size_t ci = 0; ci < ncats; ci++)
			free(cl[ci].num);
		free(cl);
	}

	/* ac3 flag by short pid */
	for (int i = 0; i < n; i++) {
		char sp[24];
		short_pid(out[i].pid, sp, sizeof(sp));
		out[i].ac3 = 0;
		for (size_t t = 0; t < sizeof(AC3_PIDS) / sizeof(AC3_PIDS[0]); t++)
			if (strcmp(AC3_PIDS[t], sp) == 0)
				out[i].ac3 = 1;
	}

	*outn = n;
	return 0;
}

static int cmp_chan_no(const void *a, const void *b)
{
	return ((const struct epg_channel *)a)->no -
	       ((const struct epg_channel *)b)->no;
}

int epg_refresh_channels(const struct auth_state *st)
{
	/* ~200KB: heap, not stack (musl thread stacks are 128KB) */
	struct epg_channel *merged = calloc(EPG_CH_MAX, sizeof(*merged));
	if (!merged)
		return -1;
	int n = 0;
	merge_channels(st, merged, &n);
	qsort(merged, (size_t)n, sizeof(merged[0]), cmp_chan_no);
	pthread_mutex_lock(&E.lock);
	memcpy(E.chans, merged, sizeof(struct epg_channel) * (size_t)n);
	E.nchans = n;
	E.last_auth_time = st->auth_time;
	pthread_mutex_unlock(&E.lock);
	free(merged);
	log_msg(LL_INFO, "epg: channel table merged, %d entries", n);
	return n;
}

/* -------- program guide (python get_programs) -------- */

static int prog_fetch(const struct auth_state *st, const struct epg_channel *ch,
		      struct epg_prog **out, int *outn)
{
	*out = NULL;
	*outn = 0;
	int64_t now_ms = (int64_t)time(NULL) * 1000;
	char t0[24], t1[24], code[96];
	snprintf(t0, sizeof(t0), "%lld", (long long)(now_ms - PROG_RANGE_BACK * 1000));
	snprintf(t1, sizeof(t1), "%lld", (long long)(now_ms + PROG_RANGE_FWD * 1000));
	snprintf(code, sizeof(code), "Umai:CHAN/%s@BESTV.SMG.SMG", ch->pid);
	char limit[8] = "2000", off[4] = "0";
	const char *form[] = {
		"action", "getChannelProg", "code", code,
		"channelID", ch->chid, "startTime", t0, "endTime", t1,
		"offset", off, "limit", limit, NULL,
	};
	struct jval *r = epg7_ajax(st, form);
	if (!r)
		return -1;
	struct jval *data = j_get(r, "data");
	struct epg_prog *progs = NULL;
	int n = 0;
	if (data && data->t == J_ARR && data->n > 0) {
		progs = calloc((size_t)data->n, sizeof(*progs));
		if (progs) {
			for (int i = 0; i < data->n; i++) {
				struct jval *p = data->items[i];
				if (!p)
					continue;
				struct epg_prog ep;
				memset(&ep, 0, sizeof(ep));
				snprintf(ep.name, sizeof(ep.name), "%s",
					 j_str(j_get(p, "name"), ""));
				ep.start_ms = strtoll(j_str(j_get(p, "startTime"), "0"),
						      NULL, 10);
				ep.end_ms = strtoll(j_str(j_get(p, "endTime"), "0"),
						    NULL, 10);
				snprintf(ep.id, sizeof(ep.id), "%s",
					 j_str(j_get(p, "ID"), ""));
				/* epg7 doubles some entries: dedup */
				int dup = 0;
				for (int k = 0; k < n; k++)
					if (progs[k].start_ms == ep.start_ms &&
					    progs[k].end_ms == ep.end_ms &&
					    strcmp(progs[k].name, ep.name) == 0)
						dup = 1;
				if (!dup)
					progs[n++] = ep;
			}
		}
	}
	j_free(r);
	*out = progs;
	*outn = n;
	return 0;
}

static struct prog_ent *cache_find(const char *pid)
{
	for (int i = 0; i < E.ncache; i++)
		if (strcmp(E.cache[i].pid, pid) == 0)
			return &E.cache[i];
	return NULL;
}

static void cache_store(const char *pid, struct epg_prog *progs, int n)
{
	if (E.ncache >= EPG_CH_MAX) {
		/* evict the oldest fetch */
		int oldest = 0;
		for (int i = 1; i < E.ncache; i++)
			if (E.cache[i].ts < E.cache[oldest].ts)
				oldest = i;
		free(E.cache[oldest].progs);
		memmove(&E.cache[oldest], &E.cache[oldest + 1],
			(size_t)(E.ncache - oldest - 1) * sizeof(E.cache[0]));
		E.ncache--;
	}
	struct prog_ent *e = &E.cache[E.ncache++];
	snprintf(e->pid, sizeof(e->pid), "%s", pid);
	e->ts = (int64_t)time(NULL);
	e->progs = progs;
	e->n = n;
}

int epg_programs(const char *pid, const struct epg_prog **progs)
{
	*progs = NULL;
	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0)
		return -1;

	pthread_mutex_lock(&E.lock);
	struct prog_ent *ent = cache_find(pid);
	if (ent && (int64_t)time(NULL) - ent->ts < PROG_CACHE_TTL) {
		*progs = ent->progs;
		int n = ent->n;
		pthread_mutex_unlock(&E.lock);
		return n;
	}
	pthread_mutex_unlock(&E.lock);

	/* ~104KB auth snapshot: heap - called from MHD worker threads */
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return -1;
	auth_mgr_snapshot(st);
	struct epg_prog *fresh = NULL;
	int n = 0;
	if (prog_fetch(st, &ch, &fresh, &n) != 0) {
		free(st);
		return -1;
	}

	pthread_mutex_lock(&E.lock);
	struct prog_ent *old = cache_find(pid);
	if (old) {
		free(old->progs);
		old->progs = fresh;
		old->n = n;
		old->ts = (int64_t)time(NULL);
		*progs = fresh;
	} else {
		cache_store(pid, fresh, n);
		*progs = fresh;
	}
	pthread_mutex_unlock(&E.lock);
	free(st);
	return n;
}

/* -------- now/next (python get_now_next) -------- */

char *epg_now_next(const char *pid)
{
	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0)
		return NULL;
	char tms[24];
	snprintf(tms, sizeof(tms), "%lld", (long long)((int64_t)time(NULL) * 1000));
	const char *form[] = { "action", "getPreCurNextProg",
			       "channelID", ch.chid, "time", tms, NULL };
	/* ~104KB auth snapshot: heap - called from MHD worker threads */
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return NULL;
	auth_mgr_snapshot(st);
	struct jval *r = epg7_ajax(st, form);
	if (!r) {
		free(st);
		return NULL;
	}
	struct jval *data = j_get(r, "data");
	char *out = data ? j_serialize(data) : NULL;
	j_free(r);
	free(st);
	return out;
}

/* -------- TVOD play URL (0.5, python get_tvod_play_url) -------- */

int epg_tvod_play_url(const char *pid, const char *playbill_id,
		      int64_t start_s, int64_t end_s, char *url_out,
		      size_t cap)
{
	*url_out = '\0';
	struct epg_channel ch;
	if (epg_find_channel(pid, &ch) != 0 || !ch.chid[0]) {
		log_msg(LL_WARN, "epg: tvod: no epg channel %s", pid);
		return -1;
	}
	/* startTime/endTime are unix SECONDS here (getChannelProg uses ms) */
	char s0[24], s1[24];
	snprintf(s0, sizeof(s0), "%lld", (long long)start_s);
	snprintf(s1, sizeof(s1), "%lld", (long long)end_s);
	const char *form[] = { "action", "getTvodPlayUrl",
			       "channelID", ch.chid,
			       "playbillID", playbill_id,
			       "startTime", s0, "endTime", s1, NULL };
	/* ~104KB auth snapshot: heap - called from MHD worker threads */
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return -1;
	auth_mgr_snapshot(st);
	struct jval *r = epg7_ajax(st, form);
	if (!r) {
		free(st);
		return -1;
	}
	const char *u = j_str(j_get(j_get(r, "data"), "playURL"), "");
	int ok = u[0] != '\0';
	if (ok)
		snprintf(url_out, cap, "%s", u);
	else
		log_msg(LL_WARN, "epg: getTvodPlayUrl(%s) returned no playURL",
			pid);
	j_free(r);
	free(st);
	return ok ? 0 : -1;
}

/* -------- search index (python build_search_index, thread pool) -------- */

static int cmp_item_desc(const void *a, const void *b)
{
	int64_t d = ((const struct search_item *)b)->start_ms -
		    ((const struct search_item *)a)->start_ms;
	return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* workers pull channels off one cursor; each fetches that channel's
 * guide (epg_programs is thread-safe: all shared state behind E.lock)
 * and appends items under cx->mtx.  E.lock is never held while cx->mtx
 * is - no nesting, no deadlock. */
struct search_ctx {
	struct search_item *items;
	int ni, max_items;
	const struct epg_channel *chans;
	int n;
	int next;
	pthread_mutex_t mtx;
};

static void *search_worker(void *arg)
{
	struct search_ctx *cx = arg;
	for (;;) {
		pthread_mutex_lock(&cx->mtx);
		if (cx->next >= cx->n || cx->ni >= cx->max_items) {
			pthread_mutex_unlock(&cx->mtx);
			return NULL;
		}
		int c = cx->next++;
		pthread_mutex_unlock(&cx->mtx);

		const struct epg_channel *ch = &cx->chans[c];
		if (!ch->chid[0])
			continue;
		const struct epg_prog *progs;
		int pn = epg_programs(ch->pid, &progs);
		if (pn <= 0 || !progs)
			continue;
		pthread_mutex_lock(&cx->mtx);
		for (int k = 0; k < pn && cx->ni < cx->max_items; k++) {
			struct search_item *it = &cx->items[cx->ni++];
			it->no = ch->no;
			snprintf(it->chname, sizeof(it->chname), "%s", ch->name);
			snprintf(it->pid, sizeof(it->pid), "%s", ch->pid);
			it->start_ms = progs[k].start_ms;
			it->end_ms = progs[k].end_ms;
			snprintf(it->name, sizeof(it->name), "%s", progs[k].name);
			snprintf(it->id, sizeof(it->id), "%s", progs[k].id);
		}
		pthread_mutex_unlock(&cx->mtx);
	}
}

static void search_build(void)
{
	pthread_mutex_lock(&E.lock);
	E.search_building = 1;
	pthread_mutex_unlock(&E.lock);

	/* ~200KB snapshot: heap (musl thread stacks are 128KB) */
	struct epg_channel *snap = malloc(sizeof(E.chans));
	if (!snap) {
		pthread_mutex_lock(&E.lock);
		E.search_building = 0;
		pthread_mutex_unlock(&E.lock);
		return;
	}
	pthread_mutex_lock(&E.lock);
	int n = E.nchans;
	memcpy(snap, E.chans, sizeof(snap[0]) * (size_t)n);
	pthread_mutex_unlock(&E.lock);

	struct search_item *items = calloc(SEARCH_ITEM_MAX, sizeof(*items));
	int ni = 0;
	if (items) {
		struct search_ctx cx = {
			.items = items, .max_items = SEARCH_ITEM_MAX,
			.chans = snap, .n = n,
			.mtx = PTHREAD_MUTEX_INITIALIZER,
		};
		/* python build_search_index runs the same fetch with a
		 * thread pool; on spawn failure the remainder is still
		 * covered by the workers already up (or run inline) */
		pthread_t th[SEARCH_WORKERS];
		int started = 0;
		for (int w = 0; w < SEARCH_WORKERS; w++) {
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, 256 * 1024);
			if (pthread_create(&th[w], &attr, search_worker, &cx) != 0) {
				pthread_attr_destroy(&attr);
				break;
			}
			pthread_attr_destroy(&attr);
			started++;
		}
		if (started)
			for (int w = 0; w < started; w++)
				pthread_join(th[w], NULL);
		else
			search_worker(&cx);
		ni = cx.ni;
		pthread_mutex_destroy(&cx.mtx);
		qsort(items, (size_t)ni, sizeof(items[0]), cmp_item_desc);
	}

	pthread_mutex_lock(&E.lock);
	free(E.items);
	E.items = items;
	E.nitems = ni;
	E.search_ts = (int64_t)time(NULL);
	E.search_done = items != NULL;
	E.search_building = 0;
	pthread_mutex_unlock(&E.lock);
	free(snap);
	log_msg(LL_INFO, "epg: search index built, %d channels %d items", n, ni);
}

char *epg_search(const char *q)
{
	pthread_mutex_lock(&E.lock);
	if (!E.search_done || !E.items) {
		pthread_mutex_unlock(&E.lock);
		return NULL;
	}
	struct sbuf sb;
	if (sb_init(&sb, 4096) != 0) {
		pthread_mutex_unlock(&E.lock);
		return NULL;
	}
	sb_printf(&sb, "[");
	int hits = 0;
	for (int i = 0; i < E.nitems && hits < 200; i++) {
		struct search_item *it = &E.items[i];
		if (!strstr(it->name, q))
			continue;
		if (hits)
			sb_printf(&sb, ",");
		sb_printf(&sb, "{\"pid\":");
		sb_json_str(&sb, it->pid);
		sb_printf(&sb, ",\"no\":%d,\"chname\":", it->no);
		sb_json_str(&sb, it->chname);
		sb_printf(&sb, ",\"name\":");
		sb_json_str(&sb, it->name);
		sb_printf(&sb, ",\"id\":");
		sb_json_str(&sb, it->id);
		sb_printf(&sb, ",\"start\":%lld,\"end\":%lld}",
			  (long long)it->start_ms, (long long)it->end_ms);
		hits++;
	}
	sb_printf(&sb, "]");
	pthread_mutex_unlock(&E.lock);
	return sb.b;
}

void epg_search_state(int *building, int64_t *age_s)
{
	pthread_mutex_lock(&E.lock);
	if (building)
		*building = E.search_building;
	if (age_s)
		*age_s = E.search_done ? (int64_t)time(NULL) - E.search_ts : -1;
	pthread_mutex_unlock(&E.lock);
}

/* -------- table accessors -------- */

int epg_channel_count(void)
{
	pthread_mutex_lock(&E.lock);
	int n = E.nchans;
	pthread_mutex_unlock(&E.lock);
	return n;
}

int epg_channels_copy(struct epg_channel *out, int max)
{
	pthread_mutex_lock(&E.lock);
	int n = E.nchans < max ? E.nchans : max;
	memcpy(out, E.chans, sizeof(out[0]) * (size_t)n);
	pthread_mutex_unlock(&E.lock);
	return n;
}

int epg_fcc_for_group(uint32_t group, int port, char *out, size_t cap)
{
	char key[64];
	snprintf(key, sizeof(key), "igmp://%s:%d",
		 inet_ntoa((struct in_addr){ group }), port);
	pthread_mutex_lock(&E.lock);
	for (int i = 0; i < E.nchans; i++) {
		if (E.chans[i].fcc[0] && strcmp(E.chans[i].igmp, key) == 0) {
			snprintf(out, cap, "%s", E.chans[i].fcc);
			pthread_mutex_unlock(&E.lock);
			return 1;
		}
	}
	pthread_mutex_unlock(&E.lock);
	return 0;
}

/* 1.1: does the channel on this live group carry AC-3 audio?  The
 * transcode wrapper (xcode) keys off this: without it those channels
 * play video-only with a dead volume control in browser MSE. */
int epg_ac3_for_group(uint32_t group, int port)
{
	char key[64];
	snprintf(key, sizeof(key), "igmp://%s:%d",
		 inet_ntoa((struct in_addr){ group }), port);
	pthread_mutex_lock(&E.lock);
	for (int i = 0; i < E.nchans; i++)
		if (E.chans[i].ac3 && strcmp(E.chans[i].igmp, key) == 0) {
			pthread_mutex_unlock(&E.lock);
			return 1;
		}
	pthread_mutex_unlock(&E.lock);
	return 0;
}

int epg_find_channel(const char *pid, struct epg_channel *out)
{
	pthread_mutex_lock(&E.lock);
	int rc = -1;
	for (int i = 0; i < E.nchans; i++) {
		if (strcmp(E.chans[i].pid, pid) == 0) {
			*out = E.chans[i];
			rc = 0;
			break;
		}
	}
	pthread_mutex_unlock(&E.lock);
	return rc;
}

/* -------- background manager -------- */

static void *mgr_thread(void *arg)
{
	(void)arg;
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return NULL;
	for (;;) {
		auth_mgr_snapshot(st);
		if (st->ok) {
			pthread_mutex_lock(&E.lock);
			int stale = E.nchans == 0 || E.last_auth_time != st->auth_time;
			int search_due = !E.search_done ||
					 (int64_t)time(NULL) - E.search_ts > SEARCH_TTL;
			pthread_mutex_unlock(&E.lock);
			if (stale)
				epg_refresh_channels(st);
			if (search_due && E.nchans > 0)
				search_build();
		}
		util_sleep_ms(30000);
	}
	free(st);
	return NULL;
}

int epg_mgr_start(void)
{
	if (E.started)
		return 0;
	E.started = 1;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if (pthread_create(&E.thread, &attr, mgr_thread, NULL) != 0) {
		E.started = 0;
		pthread_attr_destroy(&attr);
		return -1;
	}
	pthread_attr_destroy(&attr);
	pthread_detach(E.thread);
	return 0;
}
