/* See httpc.h.  Kept to blocking sockets + SO timers so it builds the
 * same on musl 1.1 (19.07) and musl 1.2 (25.12); the only place a
 * timeout must not be swallowed is connect(), which is done with a
 * non-blocking fd + poll(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "httpc.h"
#include "iptv.h"
#include "log.h"

#define HDR_MAX   (16 * 1024)
#define N_COOKIES 32

struct httpc_cookie {
	char host[64];
	char name[48];
	char value[128];
};

struct httpc {
	struct httpc_cookie cookies[N_COOKIES];
	int ncookies;
};

struct urlpart {
	char scheme[8];
	char host[64];
	int port;
	char path[HTTPC_URL_MAX];   /* path + query */
};

/* buffered reader over a socket */
struct rdbuf {
	int fd;
	unsigned char b[8192];
	size_t head, len;
};

/* open response-body reader handed out by httpc_get_stream */
struct httpc_stream {
	int fd;
	struct rdbuf rb;             /* keeps leftover bytes past the head */
	size_t remaining;            /* Content-Length bytes left */
	int chunked;
	size_t chunk_left;           /* bytes left in the current chunk */
	int started;                 /* have consumed >= one chunk-size line */
};

/* -------- small helpers -------- */

struct httpc *httpc_new(void)
{
	return calloc(1, sizeof(struct httpc));
}

void httpc_free(struct httpc *c)
{
	free(c);
}

void httpc_resp_free(struct httpc_resp *resp)
{
	if (resp) {
		free(resp->body);
		resp->body = NULL;
	}
}

void httpc_url_encode(const char *in, char *out, size_t cap)
{
	static const char hexd[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *in && o + 4 < cap; in++) {
		unsigned char ch = (unsigned char)*in;
		if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			out[o++] = (char)ch;
		} else {
			out[o++] = '%';
			out[o++] = hexd[ch >> 4];
			out[o++] = hexd[ch & 15];
		}
	}
	out[o] = '\0';
}

/* parse "http://host[:port]/path?query" */
static int url_split(const char *url, struct urlpart *u)
{
	memset(u, 0, sizeof(*u));
	const char *p = strstr(url, "://");
	if (!p)
		return -1;
	size_t sl = (size_t)(p - url);
	if (sl >= sizeof(u->scheme))
		return -1;
	memcpy(u->scheme, url, sl);
	u->scheme[sl] = '\0';
	p += 3;

	const char *slash = strchr(p, '/');
	const char *hostend = slash ? slash : p + strlen(p);
	const char *colon = memchr(p, ':', (size_t)(hostend - p));
	size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
	if (hlen == 0 || hlen >= sizeof(u->host))
		return -1;
	memcpy(u->host, p, hlen);
	u->host[hlen] = '\0';
	u->port = colon ? atoi(colon + 1) : 80;
	if (u->port <= 0 || u->port > 65535)
		return -1;
	snprintf(u->path, sizeof(u->path), "%s", slash ? slash : "/");
	return 0;
}

static void url_origin(const struct urlpart *u, char *out, size_t cap)
{
	if (u->port == 80)
		snprintf(out, cap, "http://%s", u->host);
	else
		snprintf(out, cap, "http://%s:%d", u->host, u->port);
}

void httpc_url_join(const char *base, const char *ref, char *out, size_t cap)
{
	if (!ref || !*ref) {
		snprintf(out, cap, "%s", base);
		return;
	}
	if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
		snprintf(out, cap, "%s", ref);
		return;
	}
	struct urlpart u;
	if (url_split(base, &u) != 0) {
		snprintf(out, cap, "%s", ref);
		return;
	}
	char origin[128];
	url_origin(&u, origin, sizeof(origin));
	if (ref[0] == '/') {
		snprintf(out, cap, "%s%s", origin, ref);
		return;
	}
	/* same-directory relative: strip the last path segment */
	char *slash = strrchr(u.path, '/');
	if (slash)
		*(slash + 1) = '\0';
	else
		snprintf(u.path, sizeof(u.path), "/");
	snprintf(out, cap, "%s%s%s", origin, u.path, ref);
}

/* -------- cookies -------- */

static void jar_store(struct httpc *c, const char *host, const char *setcookie)
{
	/* keep "name=value" up to ';' and store/replace per host+name */
	const char *semi = strchr(setcookie, ';');
	size_t kvlen = semi ? (size_t)(semi - setcookie) : strlen(setcookie);
	char kv[192];
	if (kvlen >= sizeof(kv))
		return;
	memcpy(kv, setcookie, kvlen);
	kv[kvlen] = '\0';

	char *eq = strchr(kv, '=');
	if (!eq)
		return;
	*eq = '\0';
	char *name = kv, *value = eq + 1;
	if (!*name || !*value)
		return;

	for (int i = 0; i < c->ncookies; i++) {
		if (strcmp(c->cookies[i].host, host) == 0 &&
		    strcmp(c->cookies[i].name, name) == 0) {
			snprintf(c->cookies[i].value, sizeof(c->cookies[i].value),
				 "%s", value);
			return;
		}
	}
	if (c->ncookies >= N_COOKIES)
		return;
	struct httpc_cookie *ck = &c->cookies[c->ncookies++];
	snprintf(ck->host, sizeof(ck->host), "%s", host);
	snprintf(ck->name, sizeof(ck->name), "%s", name);
	snprintf(ck->value, sizeof(ck->value), "%s", value);
}

static void jar_cookie_header(struct httpc *c, const char *host,
			      const char *extra, char *out, size_t cap)
{
	/* explicit Cookie header replaces the jar wholesale (urllib
	 * semantics) - merging produced duplicate JSESSIONID lines that
	 * broke the FonsView portal.jsp session init -> RTSP 401 */
	if (extra && *extra) {
		snprintf(out, cap, "%s", extra);
		return;
	}
	size_t o = 0;
	for (int i = 0; i < c->ncookies; i++) {
		if (strcmp(c->cookies[i].host, host) != 0)
			continue;
		if (o && o + 2 < cap)
			o += (size_t)snprintf(out + o, cap - o, "; ");
		o += (size_t)snprintf(out + o, cap - o, "%s=%s",
				      c->cookies[i].name, c->cookies[i].value);
		if (o >= cap)
			break;
	}
}

/* -------- socket + buffered reader -------- */

static int connect_timeout(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portstr[8];
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return -1;

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	int rc = connect(fd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	if (rc < 0) {
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLOUT)) {
			close(fd);
			return -1;
		}
	}
	/* back to blocking with IO timeouts */
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	struct timeval tv = { .tv_sec = timeout_ms / 1000,
			      .tv_usec = (timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	return fd;
}

static int rd_fill(struct rdbuf *r)
{
	if (r->len == sizeof(r->b))
		return 0;
	int n = recv(r->fd, r->b + r->len, sizeof(r->b) - r->len, 0);
	if (n < 0 && (errno == EINTR))
		return rd_fill(r);
	if (n <= 0)
		return -1;
	r->len += (size_t)n;
	return 0;
}

/* read one CRLF (or LF) terminated line without its terminator */
static int rd_line(struct rdbuf *r, char *out, size_t cap)
{
	for (;;) {
		for (size_t i = r->head; i + 1 <= r->len; i++) {
			if (r->b[i] == '\n') {
				size_t cpy = i - r->head;
				if (cpy >= cap)
					cpy = cap - 1;
				memcpy(out, r->b + r->head, cpy);
				out[cpy] = '\0';
				if (cpy && out[cpy - 1] == '\r')
					out[cpy - 1] = '\0';
				r->head = i + 1;
				return 0;
			}
		}
		if (r->len == sizeof(r->b)) {
			/* line longer than the buffer: flush what we can */
			size_t cpy = r->len - r->head;
			if (cpy >= cap)
				cpy = cap - 1;
			memcpy(out, r->b + r->head, cpy);
			out[cpy] = '\0';
			r->head = r->len = 0;
			return 0;
		}
		if (rd_fill(r) != 0)
			return -1;
	}
}

static int rd_read(struct rdbuf *r, void *dst, size_t n)
{
	unsigned char *p = dst;
	while (n) {
		if (r->head == r->len) {
			r->head = r->len = 0;
			if (rd_fill(r) != 0)
				return -1;
		}
		size_t avail = r->len - r->head;
		size_t take = avail < n ? avail : n;
		memcpy(p, r->b + r->head, take);
		r->head += take;
		p += take;
		n -= take;
	}
	return 0;
}

/* -------- body readers -------- */

static int read_body_chunked(struct rdbuf *r, char **out, size_t *outlen)
{
	char line[64];
	size_t cap = 16384;
	char *buf = malloc(cap);
	if (!buf)
		return -1;
	size_t o = 0;
	for (;;) {
		if (rd_line(r, line, sizeof(line)) != 0)
			goto fail;
		char *semi = strchr(line, ';');
		if (semi)
			*semi = '\0';
		unsigned long sz = strtoul(line, NULL, 16);
		if (sz == 0)
			break;                    /* trailer */
		if (o + sz > HTTPC_BODY_MAX)
			goto fail;
		if (o + sz > cap) {
			while (cap < o + sz && cap < HTTPC_BODY_MAX)
				cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb)
				goto fail;
			buf = nb;
		}
		if (rd_read(r, buf + o, sz) != 0)
			goto fail;
		o += sz;
		char crlf[2];
		if (rd_read(r, crlf, 2) != 0)   /* chunk CRLF */
			goto fail;
	}
	/* drain trailers until empty line (best effort) */
	while (rd_line(r, line, sizeof(line)) == 0 && line[0])
		;
	buf[o] = '\0';
	*out = buf;
	*outlen = o;
	return 0;
fail:
	free(buf);
	return -1;
}

/* -------- one request, no redirect handling --------
 * out_stream != NULL: streaming mode - on the final response the body
 * is not read; the fd + buffered reader are handed to the caller
 * instead (returns 2). */

static int one_request(struct httpc *c, const char *url, const char *method,
		       const char *const *form, const char *referer,
		       const char *origin, const char *cookie, const char *ua,
		       int timeout_ms, struct httpc_resp *resp,
		       struct httpc_stream **out_stream)
{
	struct urlpart u;
	if (url_split(url, &u) != 0) {
		log_msg(LL_ERR, "httpc: bad url '%s'", url);
		return -1;
	}
	int fd = connect_timeout(u.host, u.port, timeout_ms);
	if (fd < 0) {
		log_msg(LL_ERR, "httpc: connect %s:%d: %s", u.host, u.port,
			strerror(errno));
		return -1;
	}
	int rc = -1;

	/* request head + optional form body */
	char *body = NULL;
	size_t bodylen = 0;
	if (form) {
		size_t cap = 4096;
		body = malloc(cap);
		if (!body)
			goto out;
		for (size_t o = 0; *form; form += 2) {
			const char *name = form[0], *value = form[1] ? form[1] : "";
			char en[256], ev[2048];
			httpc_url_encode(name, en, sizeof(en));
			httpc_url_encode(value, ev, sizeof(ev));
			int w = snprintf(body + o, cap - o, "%s%s=%s", o ? "&" : "",
					 en, ev);
			if (w < 0 || (size_t)w >= cap - o) {
				log_msg(LL_ERR, "httpc: form too large");
				goto out;
			}
			o += (size_t)w;
		}
		bodylen = strlen(body);
	}

	char cookie_hdr[1024] = "";
	jar_cookie_header(c, u.host, cookie, cookie_hdr, sizeof(cookie_hdr));

	char req[8192];
	/* Host must carry the port (Host: h:p): the WebLogic auth nodes
	 * virtual-host on it - a portless Host got ottauth hderror 2901. */
	char hostport[80];
	if (u.port == 80)
		snprintf(hostport, sizeof(hostport), "%s", u.host);
	else
		snprintf(hostport, sizeof(hostport), "%s:%d", u.host, u.port);
	int head = snprintf(req, sizeof(req),
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: %s\r\n"
		"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
		"Accept-Language: zh-cn\r\n"
		"Accept-Charset: utf-8, iso-8859-1, utf-16, *;q=0.7\r\n"
		"Connection: close\r\n"
		"%s%s%s"
		"%s%s%s"
		"%s%s%s"
		"%s",
		method, u.path, hostport,
		ua ? ua : "webkit;Resolution(PAL,720P,1080P,2106P,4K)",
		referer ? "Referer: " : "", referer ? referer : "", referer ? "\r\n" : "",
		origin ? "Origin: " : "", origin ? origin : "", origin ? "\r\n" : "",
		cookie_hdr[0] ? "Cookie: " : "", cookie_hdr[0] ? cookie_hdr : "", cookie_hdr[0] ? "\r\n" : "",
		body ? "Content-Type: application/x-www-form-urlencoded\r\n" : "");
	if (head < 0 || (size_t)head + 64 >= sizeof(req))
		goto out;

	size_t reqlen = (size_t)head;
	if (body)
		reqlen += (size_t)snprintf(req + reqlen, sizeof(req) - reqlen,
					   "Content-Length: %zu\r\n", bodylen);
	if (reqlen + 3 > sizeof(req))
		goto out;
	memcpy(req + reqlen, "\r\n", 2);
	reqlen += 2;

	if (write(fd, req, reqlen) < 0)
		goto out;
	if (body && write(fd, body, bodylen) < 0)
		goto out;
	if (getenv("IPTV_DUMP_REQ")) {
		FILE *df = fopen("/tmp/creq.log", "a");
		if (df) {
			fprintf(df, "=====\n%.*s", (int)reqlen, req);
			if (body)
				fprintf(df, "%.*s\n", (int)bodylen, body);
			fclose(df);
		}
	}

	/* response */
	struct rdbuf r = { .fd = fd };
	char line[2048];
	if (rd_line(&r, line, sizeof(line)) != 0)
		goto out;
	const char *sp = strchr(line, ' ');
	if (!sp)
		goto out;
	int status = atoi(sp);
	if (status < 100 || status > 599)
		goto out;
	log_msg(LL_DEBUG, "httpc: %s %s -> %d", method, u.host, status);

	char location[HTTPC_URL_MAX] = "";
	int chunked = 0;
	size_t clen = (size_t)-1;
	char *setcookies[8] = { 0 };
	int nset = 0;

	while (rd_line(&r, line, sizeof(line)) == 0) {
		if (!line[0])
			break;                    /* end of headers */
		if (strncasecmp(line, "Location:", 9) == 0) {
			snprintf(location, sizeof(location), "%s", line + 9);
			char *s = location;
			while (*s == ' ' || *s == '\t')
				s++;
			memmove(location, s, strlen(s) + 1);
		} else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
			   strstr(line + 18, "chunked")) {
			chunked = 1;
		} else if (strncasecmp(line, "Content-Length:", 15) == 0) {
			clen = (size_t)strtoul(line + 15, NULL, 10);
		} else if (strncasecmp(line, "Set-Cookie:", 11) == 0 &&
			   nset < 8) {
			setcookies[nset++] = strdup(line + 11);
		}
	}
	for (int i = 0; i < nset; i++) {
		jar_store(c, u.host, setcookies[i]);
		free(setcookies[i]);
	}
	log_msg(LL_DEBUG, "httpc: headers done loc=%d chunked=%d clen=%zu nset=%d",
		!!location[0], chunked,
		clen == (size_t)-1 ? (size_t)0 : clen, nset);

	resp->status = status;
	resp->len = 0;
	resp->body = NULL;
	if (location[0])
		httpc_url_join(url, location, resp->url, sizeof(resp->url));
	else
		snprintf(resp->url, sizeof(resp->url), "%s", url);
	if (location[0] && (status == 301 || status == 302 || status == 303 ||
			    status == 307 || status == 308)) {
		rc = 1;                   /* caller: redirect */
		goto out;
	}

	if (out_stream) {
		struct httpc_stream *s = calloc(1, sizeof(*s));
		if (!s)
			goto out;
		s->fd = fd;
		s->rb = r;                /* struct copy keeps body leftovers */
		s->remaining = chunked ? 0 : clen;
		s->chunked = chunked;
		*out_stream = s;
		fd = -1;                  /* ownership moved to the stream */
		rc = 2;
		goto out;
	}

	if (chunked) {
		if (read_body_chunked(&r, &resp->body, &resp->len) != 0)
			goto out;
	} else if (clen != (size_t)-1) {
		if (clen > HTTPC_BODY_MAX)
			goto out;
		resp->body = malloc(clen + 1);
		if (!resp->body)
			goto out;
		if (rd_read(&r, resp->body, clen) != 0)
			goto out_free;
		resp->body[clen] = '\0';
		resp->len = clen;
	} else {
		/* EOF-delimited: grow geometrically up to the cap
		 * (+1 for the NUL: o may reach the cap) */
		size_t cap = 16384;
		resp->body = malloc(cap + 1);
		if (!resp->body)
			goto out;
		size_t o = 0;
		for (;;) {
			if (o >= cap) {
				if (cap >= HTTPC_BODY_MAX)
					break;
				cap *= 2;
				if (cap > HTTPC_BODY_MAX)
					cap = HTTPC_BODY_MAX;
				char *nb = realloc(resp->body, cap + 1);
				if (!nb)
					goto out_free;
				resp->body = nb;
			}
			if (rd_read(&r, resp->body + o, 1) != 0)
				break;
			o++;
		}
		resp->body[o] = '\0';
		resp->len = o;
	}
	rc = 0;

out_free:
	if (rc < 0) {
		free(resp->body);
		resp->body = NULL;
	}
out:
	free(body);
	if (fd >= 0)
		close(fd);
	return rc;
}

int httpc_request(struct httpc *c, const char *url, const char *method,
		  const char *const *form, const char *referer,
		  const char *origin, const char *cookie,
		  int timeout_ms, struct httpc_resp *resp)
{
	return httpc_request_ua(c, url, method, form, referer, origin, cookie,
				NULL, timeout_ms, resp);
}

int httpc_request_ua(struct httpc *c, const char *url, const char *method,
		     const char *const *form, const char *referer,
		     const char *origin, const char *cookie, const char *ua,
		     int timeout_ms, struct httpc_resp *resp)
{
	char cur[HTTPC_URL_MAX];
	snprintf(cur, sizeof(cur), "%s", url);
	char curmethod[8];
	snprintf(curmethod, sizeof(curmethod), "%s", method);

	for (int hops = 0; hops < 6; hops++) {
		memset(resp, 0, sizeof(*resp));
		int rc = one_request(c, cur, curmethod, form, referer, origin,
				     cookie, ua, timeout_ms, resp, NULL);
		if (rc < 0)
			return -1;
		if (rc == 1) {
			/* like urllib: 301/302/303 after POST -> GET */
			if (resp->status != 307 && resp->status != 308) {
				snprintf(curmethod, sizeof(curmethod), "GET");
				form = NULL;
			}
			snprintf(cur, sizeof(cur), "%s", resp->url);
			httpc_resp_free(resp);
			continue;
		}
		return 0;
	}
	log_msg(LL_ERR, "httpc: too many redirects for %s", url);
	return -1;
}

struct httpc_stream *httpc_get_stream(struct httpc *c, const char *url,
				      const char *ua, int timeout_ms,
				      int *status_out)
{
	char cur[HTTPC_URL_MAX];
	snprintf(cur, sizeof(cur), "%s", url);
	for (int hops = 0; hops < 6; hops++) {
		struct httpc_resp resp;
		memset(&resp, 0, sizeof(resp));
		struct httpc_stream *s = NULL;
		int rc = one_request(c, cur, "GET", NULL, NULL, NULL, NULL,
				     ua, timeout_ms, &resp, &s);
		if (rc == 1) {
			snprintf(cur, sizeof(cur), "%s", resp.url);
			httpc_resp_free(&resp);
			continue;
		}
		if (rc == 2) {
			if (status_out)
				*status_out = resp.status;
			return s;
		}
		return NULL;
	}
	log_msg(LL_ERR, "httpc: too many redirects for %s", url);
	return NULL;
}

ssize_t httpc_stream_read(struct httpc_stream *s, void *buf, size_t n)
{
	unsigned char *dst = buf;
	size_t got = 0;
	while (got < n) {
		if (s->chunked) {
			if (s->chunk_left == 0) {
				char line[32];
				if (s->started) {     /* CRLF after the chunk */
					char crlf[2];
					if (rd_read(&s->rb, crlf, 2) != 0)
						return -1;
				}
				if (rd_line(&s->rb, line, sizeof(line)) != 0)
					return -1;
				char *semi = strchr(line, ';');
				if (semi)
					*semi = '\0';
				unsigned long sz = strtoul(line, NULL, 16);
				if (sz == 0) {
					/* trailer section, then body end */
					while (rd_line(&s->rb, line,
						       sizeof(line)) == 0 && line[0])
						;
					return (ssize_t)got;
				}
				s->chunk_left = sz;
				s->started = 1;
			}
			size_t take = n - got;
			if (take > s->chunk_left)
				take = s->chunk_left;
			if (rd_read(&s->rb, dst + got, take) != 0)
				return -1;
			got += take;
			s->chunk_left -= take;
		} else if (s->remaining != (size_t)-1) {
			if (s->remaining == 0)
				return (ssize_t)got;  /* body end */
			size_t take = n - got;
			if (take > s->remaining)
				take = s->remaining;
			if (rd_read(&s->rb, dst + got, take) != 0)
				return -1;
			got += take;
			s->remaining -= take;
		} else {
			/* EOF-delimited */
			if (s->rb.head == s->rb.len) {
				s->rb.head = s->rb.len = 0;
				if (rd_fill(&s->rb) != 0)
					return (ssize_t)got;
			}
			size_t avail = s->rb.len - s->rb.head;
			size_t take = n - got < avail ? n - got : avail;
			memcpy(dst + got, s->rb.b + s->rb.head, take);
			s->rb.head += take;
			got += take;
		}
	}
	return (ssize_t)got;
}

size_t httpc_stream_remaining(const struct httpc_stream *s)
{
	if (!s || s->chunked)
		return 0;
	return s->remaining == (size_t)-1 ? 0 : s->remaining;
}

void httpc_stream_close(struct httpc_stream *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s);
}

/* shutdown only: wakes a recv() blocked on another thread without
 * racing the close (caller serializes close/abort against each other) */
void httpc_stream_abort(struct httpc_stream *s)
{
	if (s && s->fd >= 0)
		shutdown(s->fd, SHUT_RDWR);
}
