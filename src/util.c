#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "util.h"

int util_if_addr(const char *dev, char *out, size_t outlen)
{
	out[0] = '\0';
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", dev);
	int rc = -1;
	if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
		struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
		if (sa->sin_family == AF_INET && sa->sin_addr.s_addr != 0) {
			const char *p = inet_ntoa(sa->sin_addr);
			snprintf(out, outlen, "%s", p);
			rc = 0;
		}
	}
	close(s);
	return rc;
}

int64_t util_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void util_sleep_ms(int64_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000 };
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

/* -------- sbuf -------- */

int sb_init(struct sbuf *sb, size_t cap)
{
	if (cap < 64)
		cap = 64;
	sb->b = malloc(cap);
	if (!sb->b) {
		sb->len = sb->cap = 0;
		return -1;
	}
	sb->b[0] = '\0';
	sb->len = 0;
	sb->cap = cap;
	return 0;
}

static int sb_vprintf(struct sbuf *sb, const char *fmt, va_list ap)
{
	va_list ap2;
	va_copy(ap2, ap);
	int need = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (need < 0) {
		va_end(ap2);
		return -1;
	}
	if (sb->len + (size_t)need + 1 > sb->cap) {
		size_t ncap = sb->cap * 2;
		while (ncap < sb->len + (size_t)need + 1)
			ncap *= 2;
		char *nb = realloc(sb->b, ncap);
		if (!nb) {
			va_end(ap2);
			return -1;
		}
		sb->b = nb;
		sb->cap = ncap;
	}
	vsnprintf(sb->b + sb->len, sb->cap - sb->len, fmt, ap2);
	va_end(ap2);
	sb->len += (size_t)need;
	return 0;
}

int sb_printf(struct sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int rc = sb_vprintf(sb, fmt, ap);
	va_end(ap);
	return rc;
}

int sb_json_str(struct sbuf *sb, const char *s)
{
	if (sb_printf(sb, "\"") != 0)
		return -1;
	for (; *s; s++) {
		unsigned char ch = (unsigned char)*s;
		const char *esc = NULL;
		char ubuf[8];
		if (ch == '"')
			esc = "\\\"";
		else if (ch == '\\')
			esc = "\\\\";
		else if (ch == '\n')
			esc = "\\n";
		else if (ch == '\r')
			esc = "\\r";
		else if (ch == '\t')
			esc = "\\t";
		else if (ch < 0x20) {
			snprintf(ubuf, sizeof(ubuf), "\\u%04x", ch);
			esc = ubuf;
		}
		if (esc) {
			if (sb_printf(sb, "%s", esc) != 0)
				return -1;
			continue;
		}
		if (sb->len + 2 > sb->cap) {
			if (sb_printf(sb, "%c", ch) != 0)   /* grows */
				return -1;
			continue;
		}
		sb->b[sb->len++] = (char)ch;
		sb->b[sb->len] = '\0';
	}
	return sb_printf(sb, "\"");
}

void sb_free(struct sbuf *sb)
{
	free(sb->b);
	sb->b = NULL;
	sb->len = sb->cap = 0;
}
