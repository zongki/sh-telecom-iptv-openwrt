/* See auth.h.  HTML "parsing" here is deliberately scan-based: the
 * auth pages are machine-generated JSP with a fixed shape, the python
 * reference uses the same lenient regexes, and a full parser would be
 * a new dependency. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>

#include "auth.h"
#include "httpc.h"
#include "log.h"
#include "util.h"
#include "crypto/md5.h"
#include "crypto/aes.h"

#define AUTH_MAX_AGE  (3 * 3600)   /* session lives 4h, refresh early */
#define AUTH_RETRY_S  60

/* -------- authenticator -------- */

static void json_escape(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (; *in && o + 6 < cap; in++) {
		unsigned char ch = (unsigned char)*in;
		if (ch == '"' || ch == '\\') {
			out[o++] = '\\';
			out[o++] = (char)ch;
		} else if (ch < 0x20) {
			o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
		} else {
			out[o++] = (char)ch;
		}
	}
	out[o] = '\0';
}

/* "30.170.1.2" -> "030.170.001.002" (3-digit zero padded, the STB wire
 * format inside the authenticator JSON) */
static void pad_ip(const char *ip, char *out, size_t cap)
{
	out[0] = '\0';
	char *tmp = strdup(ip);
	int off = 0;
	for (char *p = strtok(tmp, "."); p; p = strtok(NULL, ".")) {
		off += snprintf(out + off, cap - (size_t)off, "%s%03d", off ? "," : "",
				atoi(p));
		if ((size_t)off >= cap)
			break;
	}
	free(tmp);
}

int auth_build_authenticator(const char *userid, const char *sn,
			     const char *mac, const char *stb_ip,
			     const char *encrytoken, const char *randon,
			     char *out_hex, size_t cap)
{
	char pip[24], e_user[256], e_sn[256], e_mac[256], e_tok[256];
	pad_ip(stb_ip, pip, sizeof(pip));
	json_escape(userid, e_user, sizeof(e_user));
	json_escape(sn, e_sn, sizeof(e_sn));
	json_escape(mac, e_mac, sizeof(e_mac));
	json_escape(encrytoken, e_tok, sizeof(e_tok));

	/* field order matches the python dict (json.dumps keeps it) */
	char plain[1024];
	int n = snprintf(plain, sizeof(plain),
		"{\"Randon\":\"%s\",\"EncryToken\":\"%s\",\"UserID\":\"%s\","
		"\"SN\":\"%s\",\"IP\":\"%s\",\"MAC\":\"%s\","
		"\"MagicCode\":\"CTC\",\"UpdateTime\":\"20230301175307\"}",
		randon, e_tok, e_user, e_sn, pip, e_mac);
	if (n < 0 || (size_t)n >= sizeof(plain))
		return -1;

	unsigned char key[16], cipher[512 + 16];
	iptv_md5("123456", 6, key);
	size_t clen = iptv_aes128_ecb_encrypt_pkcs7(key, plain, (size_t)n,
						    cipher, sizeof(cipher));
	if (clen == 0 || clen * 2 + 1 > cap)
		return -1;
	static const char hexd[] = "0123456789abcdef";
	for (size_t i = 0; i < clen; i++) {
		out_hex[i * 2] = hexd[cipher[i] >> 4];
		out_hex[i * 2 + 1] = hexd[cipher[i] & 15];
	}
	out_hex[clen * 2] = '\0';
	return 0;
}

static void gen_randon(char *out, size_t cap)
{
	unsigned char rb[4];
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		srand((unsigned)time(NULL));
		for (int i = 0; i < 4; i++)
			rb[i] = (unsigned char)rand();
	} else {
		if (read(fd, rb, sizeof(rb)) != (ssize_t)sizeof(rb))
			rb[0] = rb[1] = rb[2] = rb[3] = 0;
		close(fd);
	}
	snprintf(out, cap, "%c%c%c%c%c%c%c%c",
		'0' + rb[0] % 10, '0' + rb[1] % 10, '0' + rb[2] % 10,
		'0' + rb[3] % 10, '0' + rb[0] / 10 % 10, '0' + rb[1] / 10 % 10,
		'0' + rb[2] / 10 % 10, '0' + rb[3] / 10 % 10);
}

/* -------- lenient HTML helpers (regex-equivalents of auth.py) -------- */

static void copy_unescaped(const char *src, size_t n, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < cap; i++) {
		if (src[i] != '&') {
			out[o++] = src[i];
			continue;
		}
		if (!strncmp(src + i, "&amp;", 5))       { out[o++] = '&'; i += 4; }
		else if (!strncmp(src + i, "&lt;", 4))    { out[o++] = '<'; i += 3; }
		else if (!strncmp(src + i, "&gt;", 4))    { out[o++] = '>'; i += 3; }
		else if (!strncmp(src + i, "&quot;", 6))  { out[o++] = '"'; i += 5; }
		else if (!strncmp(src + i, "&#39;", 5))   { out[o++] = '\''; i += 4; }
		else out[o++] = src[i];
	}
	out[o] = '\0';
}

static const char *find_ci(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	for (const char *p = hay; *p; p++) {
		size_t i = 0;
		for (; i < nl && p[i]; i++)
			if (tolower((unsigned char)p[i]) !=
			    tolower((unsigned char)needle[i]))
				break;
		if (i == nl)
			return p;
	}
	return NULL;
}

/* attribute value of name= in a tag snippet */
static int tag_attr(const char *tag, const char *name, char *out, size_t cap)
{
	size_t nl = strlen(name);
	for (const char *p = find_ci(tag, name); p; p = find_ci(p + 1, name)) {
		if ((p > tag && !isspace((unsigned char)p[-1])) ||
		    p[nl] != '=')
			continue;
		const char *v = p + nl + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		char q = 0;
		if (*v == '"' || *v == '\'') {
			q = *v++;
		}
		const char *e = q ? strchr(v, q) : v;
		if (!e)
			e = v + strlen(v);
		copy_unescaped(v, (size_t)(e - v), out, cap);
		return 0;
	}
	out[0] = '\0';
	return -1;
}

/* var NAME = "value" (first match, either quote) */
static int get_var(const char *text, const char *name, char *out, size_t cap)
{
	size_t nl = strlen(name);
	for (const char *p = strstr(text, "var"); p; p = strstr(p + 1, "var")) {
		p += 3;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, name, nl) != 0)
			continue;
		if (!isspace((unsigned char)p[nl]) && p[nl] != '=')
			continue;
		p += nl;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '=')
			continue;
		p++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '"' && *p != '\'')
			continue;
		char q = *p++;
		const char *e = strchr(p, q);
		if (!e)
			return -1;
		copy_unescaped(p, (size_t)(e - p), out, cap);
		return 0;
	}
	out[0] = '\0';
	return -1;
}

/* top.document.location = 'url' */
static int get_top_location(const char *text, char *out, size_t cap)
{
	const char *p = find_ci(text, "top.document.location");
	if (!p)
		return -1;
	p += strlen("top.document.location");
	while (*p == ' ' || *p == '\t' || *p == '=')
		p++;
	if (*p != '"' && *p != '\'')
		return -1;
	char q = *p++;
	const char *e = strchr(p, q);
	if (!e)
		return -1;
	copy_unescaped(p, (size_t)(e - p), out, cap);
	return 0;
}

/* jsSetConfig('KEY','value') pairs (python: re with \w+ keys) */
static int jsget(const char *text, const char *key, char *out, size_t cap)
{
	size_t kl = strlen(key);
	const char *p = find_ci(text, "jsSetConfig");
	for (; p; p = find_ci(p + 1, "jsSetConfig")) {
		const char *q = strchr(p, '(');
		if (!q)
			continue;
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != '\'' && *q != '"')
			continue;
		char kq = *q++;
		const char *ke = strchr(q, kq);
		if (!ke || (size_t)(ke - q) != kl || strncmp(q, key, kl) != 0)
			continue;
		const char *v = strchr(ke + 1, ',');
		if (!v)
			continue;
		v++;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v != '\'' && *v != '"')
			continue;
		char vq = *v++;
		const char *ve = strchr(v, vq);
		if (!ve)
			continue;
		copy_unescaped(v, (size_t)(ve - v), out, cap);
		return 0;
	}
	out[0] = '\0';
	return -1;
}

/* "key":"value" from JSON-ish bodies */
static int json_get(const char *text, const char *key, char *out, size_t cap)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(text, pat);
	if (!p)
		return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == ':')
		p++;
	if (*p != '"')
		return -1;
	p++;
	const char *e = p;
	while (*e && *e != '"') {
		if (*e == '\\' && e[1])
			e++;
		e++;
	}
	copy_unescaped(p, (size_t)(e - p), out, cap);
	return 0;
}

/* -------- form walking -------- */

#define FORM_MAX_FIELDS 16
/* value width must hold the injected authenticator (448 hex chars +
 * NUL) - at 192 the ottauth POST shipped a truncated token */
#define FORM_VAL_MAX 512

struct hform {
	char action[HTTPC_URL_MAX];
	char method[8];
	int nfields;
	char names[FORM_MAX_FIELDS][48];
	char values[FORM_MAX_FIELDS][FORM_VAL_MAX];
};

static int form_fill(const char *text, const char *base_url,
		     struct hform *f, int *found)
{
	memset(f, 0, sizeof(*f));
	const char *p = find_ci(text, "<form");
	if (!p) {
		*found = 0;
		return 0;
	}
	p += 5;
	const char *end = find_ci(p, ">");
	if (!end)
		return -1;
	char tag[512];
	size_t tl = (size_t)(end - p);
	if (tl >= sizeof(tag))
		tl = sizeof(tag) - 1;
	memcpy(tag, p, tl);
	tag[tl] = '\0';

	tag_attr(tag, "action", f->action, sizeof(f->action));
	tag_attr(tag, "method", f->method, sizeof(f->method));
	if (!f->method[0])
		snprintf(f->method, sizeof(f->method), "get");
	for (char *m = f->method; *m; m++)
		*m = (char)toupper((unsigned char)*m);
	if (!f->action[0])
		snprintf(f->action, sizeof(f->action), "%s", base_url);
	else {
		char abs[HTTPC_URL_MAX];
		httpc_url_join(base_url, f->action, abs, sizeof(abs));
		snprintf(f->action, sizeof(f->action), "%s", abs);
	}

	const char *close = find_ci(end, "</form");
	const char *lim = close ? close : end + strlen(end);
	for (const char *ip = find_ci(end, "<input"); ip && ip < lim;
	     ip = find_ci(ip + 1, "<input")) {
		ip += 7;
		const char *iend = find_ci(ip, ">");
		if (!iend || iend > lim)
			break;
		size_t il = (size_t)(iend - ip);
		char itag[384];
		if (il >= sizeof(itag))
			il = sizeof(itag) - 1;
		memcpy(itag, ip, il);
		itag[il] = '\0';
		char name[48];
		if (tag_attr(itag, "name", name, sizeof(name)) != 0 || !name[0])
			continue;
		if (f->nfields >= FORM_MAX_FIELDS)
			break;
		snprintf(f->names[f->nfields], sizeof(f->names[0]), "%s", name);
		tag_attr(itag, "value", f->values[f->nfields],
			 sizeof(f->values[0]));
		f->nfields++;
	}
	*found = 1;
	return 0;
}

/* form containing substr in action, after skipping past form #skip */
static int form_find(const char *text, const char *base_url, const char *substr,
		     struct hform *f)
{
	int found = 0;
	const char *cursor = text;
	for (int i = 0; ; i++) {
		if (form_fill(cursor, base_url, f, &found) != 0 || !found)
			return -1;
		if (strstr(f->action, substr))
			return 0;
		/* advance past this form */
		const char *next = find_ci(cursor, "</form");
		if (!next)
			return -1;
		cursor = next + 6;
	}
}

static void form_add(struct hform *f, const char *name, const char *value)
{
	if (f->nfields >= FORM_MAX_FIELDS)
		return;
	snprintf(f->names[f->nfields], sizeof(f->names[0]), "%s", name);
	snprintf(f->values[f->nfields], sizeof(f->values[0]), "%s", value);
	f->nfields++;
}

/* set a field, replacing an existing one in place.  The real ottauth
 * form carries an EMPTY hidden "authenticator" as its first input;
 * appending a second copy made the server read the empty first value
 * and reject the login with hderror 2901 (getParameter takes the
 * first occurrence). */
static void form_set(struct hform *f, const char *name, const char *value)
{
	for (int i = 0; i < f->nfields; i++) {
		if (strcmp(f->names[i], name) == 0) {
			snprintf(f->values[i], sizeof(f->values[0]), "%s", value);
			return;
		}
	}
	form_add(f, name, value);
}

/* NULL-terminated name/value array for httpc_request (caller frees) */
static const char **form_array(const struct hform *f)
{
	const char **kv = calloc((size_t)f->nfields * 2 + 1, sizeof(char *));
	if (!kv)
		return NULL;
	for (int i = 0; i < f->nfields; i++) {
		kv[i * 2] = f->names[i];
		kv[i * 2 + 1] = f->values[i];
	}
	return kv;
}

/* -------- ottauth channelArray (python parse_channels) --------
 * Attributes appear in a fixed order per entry; scan sequentially.
 * The leading-char guard keeps "ChannelID" from matching inside
 * "UserChannelID". */
/* attribute value of name= after *cursor (sequential scan); stop is an
 * optional entry boundary: a match at/after it means the field is
 * absent from the current entry (optional trailing attributes like
 * ChannelFCCIP must not drag the parser into the next one). */
static int attr_seq_lim(const char *base, const char **cursor,
			const char *name, const char *stop,
			char *out, size_t cap)
{
	size_t nl = strlen(name);
	for (const char *p = *cursor; (p = find_ci(p, name)) != NULL; p++) {
		if (stop && p >= stop)
			return -1;              /* not in this entry */
		if (p > base &&
		    (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
			continue;               /* substring of a longer name */
		if (strncmp(p + nl, "=\"", 2) != 0)
			continue;
		const char *v = p + nl + 2;
		const char *e = strchr(v, '"');
		if (!e)
			return -1;
		copy_unescaped(v, (size_t)(e - v), out, cap);
		*cursor = e + 1;
		return 0;
	}
	return -1;
}

static int attr_seq(const char *base, const char **cursor, const char *name,
		    char *out, size_t cap)
{
	return attr_seq_lim(base, cursor, name, NULL, out, cap);
}

/* attr_seq for optional attributes: only accepts a match inside the
 * current entry (before the next ChannelID="), so an entry that lacks
 * the field never drags the sequential parser into the next one. */
static int attr_seq_opt(const char *base, const char **cursor, const char *name,
			char *out, size_t cap)
{
	const char *limit = find_ci(*cursor, "ChannelID=\"");
	if (!limit)
		limit = *cursor + strlen(*cursor);
	return attr_seq_lim(base, cursor, name, limit, out, cap);
}

static void parse_raw_channels(const char *text, struct auth_state *st)
{
	const char *cur = text;
	while (st->nraw < AUTH_RAW_CH_MAX) {
		struct auth_raw_channel rc;
		char ts[8] = "", sdp[192] = "", cid[48] = "", ucid[48] = "";
		memset(&rc, 0, sizeof(rc));
		if (attr_seq(text, &cur, "ChannelID", cid, sizeof(cid)) != 0 ||
		    attr_seq(text, &cur, "UserChannelID", ucid, sizeof(ucid)) != 0 ||
		    attr_seq(text, &cur, "ChannelURL", rc.igmp, sizeof(rc.igmp)) != 0 ||
		    attr_seq(text, &cur, "TimeShift", ts, sizeof(ts)) != 0 ||
		    attr_seq(text, &cur, "ChannelSDP", sdp, sizeof(sdp)) != 0 ||
		    attr_seq(text, &cur, "TimeShiftURL", rc.timeshift_url,
			     sizeof(rc.timeshift_url)) != 0)
			break;
		static const char live[] = "/SHDXFH_live/";
		const char *m = strstr(sdp, live);
		if (!m)
			continue;               /* non-live entry: skip */
		const char *dig = m + sizeof(live) - 1;
		size_t dl = 0;
		while (isdigit((unsigned char)dig[dl]))
			dl++;
		if (dl == 0 || dl >= sizeof(rc.pid))
			continue;
		memcpy(rc.pid, dig, dl);
		rc.pid[dl] = '\0';
		rc.ts = strcmp(ts, "1") == 0;
		/* operator-designated FCC server for this channel (r16);
		 * trailing, and absent on ~1 in 9 entries */
		char fport[8] = "", fip[24] = "";
		attr_seq_opt(text, &cur, "ChannelFCCPort", fport, sizeof(fport));
		attr_seq_opt(text, &cur, "ChannelFCCIP", fip, sizeof(fip));
		if (fip[0] && fport[0])
			snprintf(rc.fcc, sizeof(rc.fcc), "%s:%s", fip, fport);
		st->raw[st->nraw++] = rc;
	}
}

/* -------- the chain -------- */

#define FAIL(...) do { \
	snprintf(st->err, sizeof(st->err), __VA_ARGS__); \
	st->ok = 0; \
	log_msg(LL_ERR, "auth: %s", st->err); \
	goto fail; \
} while (0)

int auth_run_full(const struct iptv_cfg *cfg, const char *stb_ip,
		  struct auth_state *st)
{
	memset(st, 0, sizeof(*st));
	struct httpc *c = httpc_new();
	if (!c)
		return -1;
	struct httpc_resp resp;
	struct hform f;
	int found = 0;
	int rc = -1;

	/* 1. pre4kLogAuth */
	char q_user[128], q_sn[128], u1[HTTPC_URL_MAX], origin[128];
	httpc_url_encode(cfg->userid, q_user, sizeof(q_user));
	httpc_url_encode(cfg->sn, q_sn, sizeof(q_sn));
	snprintf(u1, sizeof(u1), "http://%s/iptv3a/4kLogAuth.do", cfg->auth_host);
	snprintf(origin, sizeof(origin), "http://%s", cfg->auth_host);
	char url[HTTPC_URL_MAX * 2];   /* u1 + encoded query, u1 is URL_MAX */
	snprintf(url, sizeof(url),
		 "%s?Action=Login&UserID=%s&SN=%s&Type=iptv4k&Mode=MENU.SMG-4K&FCCSupport=1",
		 u1, q_user, q_sn);
	log_msg(LL_DEBUG, "auth: step1 pre4kLogAuth");
	if (httpc_request(c, url, "GET", NULL, NULL, NULL, NULL, 15000, &resp) != 0)
		FAIL("pre4kLogAuth request failed");
	log_msg(LL_DEBUG, "auth: step1 done status=%d len=%zu", resp.status, resp.len);
	char *text = resp.body;
	snprintf(url, sizeof(url), "%s", resp.url);

	/* 2. r4kLogAuth: submit the returned form */
	log_msg(LL_DEBUG, "auth: step2 r4kLogAuth parse form");
	if (form_fill(text, url, &f, &found) != 0 || !found)
		FAIL("4kLogAuth.do: no form");
	const char **kv = form_array(&f);
	httpc_resp_free(&resp);
	log_msg(LL_DEBUG, "auth: step2 post %s (%d fields)", f.action, f.nfields);
	int r = httpc_request(c, f.action, f.method, kv, u1, origin, NULL,
			      15000, &resp);
	free(kv);
	if (r != 0)
		FAIL("r4kLogAuth request failed");
	log_msg(LL_DEBUG, "auth: step2 done status=%d len=%zu", resp.status, resp.len);
	text = resp.body;
	snprintf(url, sizeof(url), "%s", resp.url);
	char encrytoken[192];
	if (get_var(text, "encrytoken", encrytoken, sizeof(encrytoken)) != 0)
		FAIL("no encrytoken in r4kLogAuth");

	/* 3. ottauth: form + authenticator */
	if (form_fill(text, url, &f, &found) != 0 || !found)
		FAIL("r4kLogAuth: no form");
	char randon[16];
	gen_randon(randon, sizeof(randon));
	char authenticator[1030];
	if (auth_build_authenticator(cfg->userid, cfg->sn, cfg->mac, stb_ip,
				     encrytoken, randon, authenticator,
				     sizeof(authenticator)) != 0)
		FAIL("authenticator build failed");
	form_set(&f, "authenticator", authenticator);
	/* referer netloc of the *final* r4kLogAuth url */
	char host[64];
	if (sscanf(resp.url, "http://%63[^/]", host) != 1)
		FAIL("bad r4kLogAuth url");
	char ref[HTTPC_URL_MAX], org[128];
	snprintf(ref, sizeof(ref), "http://%s/iptv3a/4kLogAuth.do", host);
	snprintf(org, sizeof(org), "http://%s", host);
	char ottauth_url[HTTPC_URL_MAX];
	snprintf(ottauth_url, sizeof(ottauth_url), "%s", f.action);
	kv = form_array(&f);
	httpc_resp_free(&resp);
	log_msg(LL_DEBUG, "auth: step3 ottauth post %s (%d fields)", f.action, f.nfields);
	r = httpc_request(c, f.action, f.method, kv, ref, org, NULL, 15000, &resp);
	free(kv);
	if (r != 0)
		FAIL("ottauth request failed");
	text = resp.body;
	snprintf(url, sizeof(url), "%s", resp.url);
	log_msg(LL_DEBUG, "auth: step3 done status=%d len=%zu", resp.status, resp.len);
	if (get_var(text, "usertoken", st->usertoken, sizeof(st->usertoken)) != 0) {
		FILE *df = fopen("/tmp/ottauth_fail.html", "w");
		if (df) {
			fputs(text, df);
			fclose(df);
		}
		FAIL("ottauth: no usertoken (dumped /tmp/ottauth_fail.html, %zu bytes)",
		     resp.len);
	}
	get_var(text, "epggroup", st->epggroup, sizeof(st->epggroup));
	parse_raw_channels(text, st);
	log_msg(LL_INFO, "auth: ottauth OK usertoken=%.16s... (%d raw channels)",
		st->usertoken, st->nraw);

	/* 4. EPG index.jsp -> loadbalance */
	log_msg(LL_DEBUG, "auth: step4 find index.jsp form");
	if (form_find(text, url, "index.jsp", &f) != 0)
		FAIL("no index.jsp form");
	kv = form_array(&f);
	httpc_resp_free(&resp);
	r = httpc_request(c, f.action, "POST", kv, ottauth_url, NULL, NULL,
			  15000, &resp);
	free(kv);
	if (r != 0)
		FAIL("EPG index request failed");
	text = resp.body;
	snprintf(url, sizeof(url), "%s", resp.url);
	log_msg(LL_DEBUG, "auth: step4 done status=%d len=%zu", resp.status, resp.len);
	char lb[HTTPC_URL_MAX];
	if (get_top_location(text, lb, sizeof(lb)) == 0 && lb[0]) {
		char next[HTTPC_URL_MAX];
		httpc_url_join(url, lb, next, sizeof(next));
		httpc_resp_free(&resp);
		if (httpc_request(c, next, "GET", NULL, url, NULL, NULL,
				  15000, &resp) != 0)
			FAIL("loadbalance request failed");
		text = resp.body;
		snprintf(url, sizeof(url), "%s", resp.url);
	}

	/* 5. funcportalauth */
	log_msg(LL_DEBUG, "auth: step5 find portalauth form");
	if (form_find(text, url, "portalauth", &f) != 0)
		FAIL("no portalauth form");
	kv = form_array(&f);
	httpc_resp_free(&resp);
	r = httpc_request(c, f.action, f.method, kv, url, NULL, NULL,
			  15000, &resp);
	free(kv);
	if (r != 0)
		FAIL("funcportalauth request failed");
	text = resp.body;
	snprintf(url, sizeof(url), "%s", resp.url);
	log_msg(LL_DEBUG, "auth: step5 done status=%d len=%zu", resp.status, resp.len);

	/* 6. jsSetConfig session params */
	char framecode[48], ip_port[64], cookie[192];
	if (jsget(text, "SessionID", st->session_id, sizeof(st->session_id)) != 0 ||
	    jsget(text, "framecode", framecode, sizeof(framecode)) != 0 ||
	    jsget(text, "IpPort", ip_port, sizeof(ip_port)) != 0 ||
	    !framecode[0] || !ip_port[0])
		FAIL("no SessionID/framecode/IpPort");
	snprintf(st->epg_host_url, sizeof(st->epg_host_url),
		 "http://%s/iptvepg/%s", ip_port, framecode);

	/* portal.jsp initializes the server-side program guide state.
	 * Must be epg_host_url + "/portal.jsp" - the bare frame URL
	 * redirects to a placeholder and FonsView then rejects the
	 * ottauth AuthInfo at RTSP DESCRIBE with 401. */
	snprintf(cookie, sizeof(cookie), "JSESSIONID=%s", st->session_id);
	httpc_resp_free(&resp);
	char portal_url[HTTPC_URL_MAX];
	snprintf(portal_url, sizeof(portal_url), "%s/portal.jsp", st->epg_host_url);
	log_msg(LL_DEBUG, "auth: step6 portal.jsp %s", portal_url);
	if (httpc_request(c, portal_url, "GET", NULL, NULL, NULL, cookie,
			  15000, &resp) != 0)
		FAIL("portal.jsp request failed");
	httpc_resp_free(&resp);

	/* 7. AuthByAjax -> bims_user_token */
	char auth_url[HTTPC_URL_MAX];
	snprintf(auth_url, sizeof(auth_url),
		 "%s/service/auth/AuthByAjax.jsp?action=auth", st->epg_host_url);
	log_msg(LL_DEBUG, "auth: step7 AuthByAjax");
	if (httpc_request(c, auth_url, "GET", NULL, NULL, NULL, cookie,
			  15000, &resp) != 0)
		FAIL("AuthByAjax request failed");
	json_get(resp.body, "bimsUserToken", st->bims_user_token,
		 sizeof(st->bims_user_token));

	st->auth_time = time(NULL);
	st->ok = 1;
	st->err[0] = '\0';
	rc = 0;
	log_msg(LL_INFO, "auth: chain OK (framecode %s @ %s)", framecode, ip_port);

fail:
	httpc_resp_free(&resp);
	httpc_free(c);
	return rc;
}

/* -------- background manager -------- */

static struct {
	pthread_mutex_t lock;
	struct auth_state st;
	struct iptv_cfg cfg;
	int enabled;
	int stop;
	pthread_t thread;
} M = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *mgr_thread(void *arg)
{
	(void)arg;
	/* struct auth_state is ~100KB: way past the musl default thread
	 * stack (128KB) once auth_run_full's own frames stack on top - it
	 * must live on the heap (the r2 board segfault was exactly this). */
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return NULL;
	while (!M.stop) {
		char ip[48];
		if (util_if_addr(M.cfg.device, ip, sizeof(ip)) != 0 && !M.cfg.bind_addr[0])
			ip[0] = '\0';
		const char *stb_ip = M.cfg.bind_addr[0] ? M.cfg.bind_addr : ip;
		int rc = auth_run_full(&M.cfg, stb_ip, st);
		pthread_mutex_lock(&M.lock);
		M.st = *st;
		pthread_mutex_unlock(&M.lock);
		if (rc == 0) {
			/* sleep in 5s steps so stop stays responsive
			 * (AUTH_MAX_AGE is in SECONDS: /5 steps, not /5000 -
			 * integer division made this a 10s re-auth loop) */
			for (int i = 0; i < AUTH_MAX_AGE / 5 && !M.stop; i++)
				util_sleep_ms(5000);
			continue;
		}
		for (int i = 0; i < AUTH_RETRY_S && !M.stop; i++)
			util_sleep_ms(1000);
	}
	free(st);
	return NULL;
}

int auth_mgr_force_refresh(struct auth_state *out)
{
	if (!M.enabled)
		return -1;
	struct iptv_cfg cfg;
	pthread_mutex_lock(&M.lock);
	cfg = M.cfg;
	pthread_mutex_unlock(&M.lock);

	char ip[48];
	if (util_if_addr(cfg.device, ip, sizeof(ip)) != 0 && !cfg.bind_addr[0])
		ip[0] = '\0';
	const char *stb_ip = cfg.bind_addr[0] ? cfg.bind_addr : ip;

	/* heap: runs on MHD worker threads, same musl stack limits */
	struct auth_state *st = malloc(sizeof(*st));
	if (!st)
		return -1;
	int rc = auth_run_full(&cfg, stb_ip, st);
	pthread_mutex_lock(&M.lock);
	M.st = *st;
	pthread_mutex_unlock(&M.lock);
	if (out)
		*out = *st;
	log_msg(LL_INFO, "auth: forced refresh %s",
		rc == 0 ? "ok" : st->err);
	free(st);
	return rc;
}

int auth_mgr_start(const struct iptv_cfg *cfg)
{
	if (M.enabled)
		return 0;
	if (!cfg->userid[0] || !cfg->sn[0] || !cfg->mac[0] || !cfg->auth_host[0])
		return -1;
	M.cfg = *cfg;
	M.enabled = 1;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	if (pthread_create(&M.thread, &attr, mgr_thread, NULL) != 0) {
		M.enabled = 0;
		pthread_attr_destroy(&attr);
		return -1;
	}
	pthread_attr_destroy(&attr);
	pthread_detach(M.thread);
	log_msg(LL_INFO, "auth: manager started (%s)", cfg->auth_host);
	return 0;
}

int auth_mgr_enabled(void)
{
	return M.enabled;
}

void auth_mgr_snapshot(struct auth_state *out)
{
	pthread_mutex_lock(&M.lock);
	*out = M.st;
	pthread_mutex_unlock(&M.lock);
}
