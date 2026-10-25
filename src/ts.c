/* See ts.h.  One connection per stream; the DESCRIBE chain uses one
 * short-lived connection per hop exactly like the python reference. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

#include "ts.h"
#include "iptv.h"
#include "log.h"

#define TS_HOP_MAX   5
#define TS_FRAME_MAX (7 * 188)      /* one RTP-sized TS batch */

struct ts_stream {
	int fd;
	/* interleaved reassembly */
	unsigned char buf[65536];
	size_t head, len;
	unsigned char frame[16 * 1024];
	size_t flen;
	int dead;
};

/* -------- small helpers -------- */

static int fail(char *err, size_t errcap, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errcap, fmt, ap);
	va_end(ap);
	log_msg(LL_WARN, "ts: %s", err);
	return -1;
}

/* rtsp://host[:port]/... -> host, port (default 554), returns path
 * start pointer or NULL */
static const char *rtsp_split(const char *url, char *host, size_t hcap, int *port)
{
	if (strncmp(url, "rtsp://", 7) != 0)
		return NULL;
	const char *p = url + 7;
	const char *slash = strchr(p, '/');
	const char *hostend = slash ? slash : p + strlen(p);
	const char *colon = memchr(p, ':', (size_t)(hostend - p));
	size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
	if (hlen == 0 || hlen >= hcap)
		return NULL;
	memcpy(host, p, hlen);
	host[hlen] = '\0';
	*port = colon ? atoi(colon + 1) : 554;
	if (*port <= 0 || *port > 65535)
		return NULL;
	return slash ? slash : p + strlen(p);
}

static int tcp_connect(const char *host, int port, int timeout_ms)
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
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	return fd;
}

static void set_rcvtimeo(int fd, int ms)
{
	struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const char *data, size_t len)
{
	while (len) {
		ssize_t n = send(fd, data, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

/* read a full RTSP response head (up to \r\n\r\n); leftover bytes
 * stay in rbuf (head/len) for the $ frames that follow */
static int read_response(int fd, unsigned char *rbuf, size_t *rlen,
			 size_t rcap, char *out, size_t outcap)
{
	set_rcvtimeo(fd, 10000);
	for (;;) {
		/* check for terminator in what we have */
		for (size_t i = 0; i + 3 < *rlen; i++) {
			if (rbuf[i] == '\r' && rbuf[i + 1] == '\n' &&
			    rbuf[i + 2] == '\r' && rbuf[i + 3] == '\n') {
				size_t cpy = i + 4;
				if (cpy > outcap - 1)
					cpy = outcap - 1;
				memcpy(out, rbuf, cpy);
				out[cpy] = '\0';
				memmove(rbuf, rbuf + i + 4, *rlen - (i + 4));
				*rlen -= i + 4;
				return 0;
			}
		}
		if (*rlen >= rcap)
			return -1;          /* head too large */
		ssize_t n = recv(fd, rbuf + *rlen, rcap - *rlen, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		*rlen += (size_t)n;
	}
}

/* header value for "Name:" (case-insensitive, first occurrence) */
static int resp_header(const char *resp, const char *name,
		       char *out, size_t cap)
{
	size_t nl = strlen(name);
	const char *p = resp;
	while ((p = strstr(p, name)) != NULL) {
		if ((p == resp || p[-1] == '\n') && p[nl] == ':') {
			p += nl + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			const char *e = p;
			while (*e && *e != '\r' && *e != '\n')
				e++;
			size_t cpy = (size_t)(e - p) < cap - 1 ? (size_t)(e - p) : cap - 1;
			memcpy(out, p, cpy);
			out[cpy] = '\0';
			return 0;
		}
		p += nl;
	}
	return -1;
}

/* -------- resolve: DESCRIBE 302 chain -------- */

int ts_resolve(const char *base_url, char *content_base, size_t cb_cap,
	       char *err, size_t errcap)
{
	char url[1024];
	snprintf(url, sizeof(url), "%s", base_url);
	for (int hop = 1; hop <= TS_HOP_MAX; hop++) {
		char host[64];
		int port;
		const char *path = rtsp_split(url, host, sizeof(host), &port);
		if (!path)
			return fail(err, errcap, "bad rtsp url '%.80s'", url);
		int fd = tcp_connect(host, port, 10000);
		if (fd < 0)
			return fail(err, errcap, "connect %s:%d failed", host, port);

		char req[1400];
		int rl = snprintf(req, sizeof(req),
				  "DESCRIBE %s RTSP/1.0\r\nCSeq: %d\r\n"
				  "User-Agent: iptvd\r\n"
				  "Accept: application/sdp\r\n\r\n",
				  url, hop);
		unsigned char rbuf[8192];
		size_t rlen = 0;
		char resp[4096];
		int rc = -1;
		if (send_all(fd, req, (size_t)rl) == 0)
			rc = read_response(fd, rbuf, &rlen, sizeof(rbuf), resp, sizeof(resp));
		close(fd);
		if (rc != 0)
			return fail(err, errcap, "DESCRIBE hop %d: no response", hop);

		if (strncmp(resp, "RTSP/1.0 401", 12) == 0 ||
		    strncmp(resp, "RTSP/1.0 454", 12) == 0)
			return fail(err, errcap, "credentials expired (%.60s)",
				    strtok(resp, "\r\n"));
		char loc[1024];
		if (resp_header(resp, "Location", loc, sizeof(loc)) == 0 && loc[0]) {
			snprintf(url, sizeof(url), "%s", loc);
			continue;
		}
		if (strncmp(resp, "RTSP/1.0 200", 12) == 0 &&
		    resp_header(resp, "Content-Base", content_base, cb_cap) == 0 &&
		    content_base[0])
			return 0;
		return fail(err, errcap, "DESCRIBE hop %d: %.120s", hop,
			    strtok(resp, "\r\n"));
	}
	return fail(err, errcap, "302 redirects exceed %d hops", TS_HOP_MAX);
}

/* -------- DURATION window -------- */

static int64_t parse_rtsp_utc(const char *s)
{
	/* YYYYMMDDTHHMMSSZ -> epoch (UTC), field-by-field to stay TZ-free */
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	if (sscanf(s, "%4d%2d%2dT%2d%2d%2d", &tm.tm_year, &tm.tm_mon,
		   &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		return -1;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	return (int64_t)timegm(&tm);
}

int ts_parse_window(const char *content_base, int64_t *start, int64_t *end)
{
	*start = *end = -1;
	const char *d = strstr(content_base, "DURATION=");
	if (!d)
		return 0;
	d += 9;
	/* <8>T<6>Z-<8>T<6>Z (16 chars per side, buffer rounded up) */
	char a[24] = "", b[24] = "";
	if (sscanf(d, "%23[^-]-%23s", a, b) != 2)
		return 0;
	int64_t s = parse_rtsp_utc(a);
	int64_t e = parse_rtsp_utc(b);
	if (s < 0 || e < 0)
		return 0;
	*start = s;
	*end = e;
	return 1;
}

/* -------- open: SETUP + PLAY -------- */

struct ts_stream *ts_open(const char *content_base, int64_t start_epoch,
			  int back_seconds, char *err, size_t errcap)
{
	char host[64];
	int port;
	if (!rtsp_split(content_base, host, sizeof(host), &port)) {
		fail(err, errcap, "bad content-base");
		return NULL;
	}
	int fd = tcp_connect(host, port, 15000);
	if (fd < 0) {
		fail(err, errcap, "connect %s:%d failed", host, port);
		return NULL;
	}
	set_rcvtimeo(fd, 15000);

	struct ts_stream *s = calloc(1, sizeof(*s));
	if (!s) {
		close(fd);
		return NULL;
	}
	s->fd = fd;
	unsigned char rbuf[8192];
	size_t rlen = 0;
	char resp[4096];

	char req[1600];
	int rl = snprintf(req, sizeof(req),
			  "SETUP %s RTSP/1.0\r\nCSeq: 10\r\n"
			  "User-Agent: iptvd\r\n"
			  "Transport: MP2T/TCP;interleaved=0-1\r\n\r\n",
			  content_base);
	if (send_all(fd, req, (size_t)rl) != 0 ||
	    read_response(fd, rbuf, &rlen, sizeof(rbuf), resp, sizeof(resp)) != 0 ||
	    strncmp(resp, "RTSP/1.0 200", 12) != 0) {
		fail(err, errcap, "SETUP failed: %.120s",
		     rlen ? strtok(resp, "\r\n") : "no response");
		goto fail;
	}
	char sess[128] = "";
	resp_header(resp, "Session", sess, sizeof(sess));
	char *semi = strchr(sess, ';');
	if (semi)
		*semi = '\0';

	char range[128] = "";
	if (start_epoch > 0 || back_seconds > 0) {
		int64_t t = start_epoch > 0 ? start_epoch : (int64_t)time(NULL) - back_seconds;
		struct tm tm;
		gmtime_r(&t, &tm);
		char iso[32];
		strftime(iso, sizeof(iso), "%Y%m%dT%H%M%SZ", &tm);
		snprintf(range, sizeof(range), "Range: clock=%s-\r\n", iso);
	}
	rl = snprintf(req, sizeof(req),
		      "PLAY %s RTSP/1.0\r\nCSeq: 11\r\n"
		      "User-Agent: iptvd\r\nSession: %s\r\n%s\r\n",
		      content_base, sess, range);
	if (send_all(fd, req, (size_t)rl) != 0 ||
	    read_response(fd, rbuf, &rlen, sizeof(rbuf), resp, sizeof(resp)) != 0 ||
	    strncmp(resp, "RTSP/1.0 200", 12) != 0) {
		fail(err, errcap, "PLAY failed: %.120s", strtok(resp, "\r\n"));
		goto fail;
	}
	/* bytes already buffered past the PLAY head are stream data */
	if (rlen > sizeof(s->buf)) {
		fail(err, errcap, "stream overrun");
		goto fail;
	}
	memcpy(s->buf, rbuf, rlen);
	s->len = rlen;
	return s;

fail:
	ts_close(s);
	return NULL;
}

/* -------- $ interleaved framing -> raw TS -------- */

int ts_read(struct ts_stream *s, void *out, size_t max)
{
	unsigned char *dst = out;
	size_t got = 0;

	for (;;) {
		/* leftover partial frame from a previous full caller buffer */
		if (s->flen) {
			size_t take = s->flen < max - got ? s->flen : max - got;
			memcpy(dst + got, s->frame, take);
			got += take;
			s->flen -= take;
			memmove(s->frame, s->frame + take, s->flen);
			if (got >= max)
				return (int)got;
		}
		/* whole frames sitting in the socket buffer */
		while (s->len - s->head >= 4) {
			unsigned char *p = s->buf + s->head;
			if (p[0] != '$') {
				s->head++;   /* resync past server noise */
				continue;
			}
			size_t fln = ((size_t)p[2] << 8) | p[3];
			if (s->len - s->head < 4 + fln)
				break;
			size_t take = fln < max - got ? fln : max - got;
			memcpy(dst + got, p + 4, take);
			got += take;
			s->head += 4 + fln;
			if (take < fln) {          /* caller buffer filled mid-frame */
				s->flen = fln - take;
				memcpy(s->frame, p + 4 + take, s->flen);
				return (int)got;
			}
			if (got >= max)
				return (int)got;
		}
		if (got)
			return (int)got;          /* flush what was parsed */

		if (s->head == s->len)
			s->head = s->len = 0;
		ssize_t n = recv(s->fd, s->buf + s->len, sizeof(s->buf) - s->len, 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			s->dead = 1;
			return got ? (int)got : -1;
		}
		s->len += (size_t)n;
	}
}

void ts_close(struct ts_stream *s)
{
	if (!s)
		return;
	shutdown(s->fd, 2);
	close(s->fd);
	free(s);
}

/* 1.1: shutdown only, to unblock a concurrently blocked ts_read (the
 * xcode pump); ts_close still owns the close and the free. */
void ts_halt(struct ts_stream *s)
{
	if (s)
		shutdown(s->fd, 2);
}
