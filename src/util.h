#ifndef IPTV_UTIL_H
#define IPTV_UTIL_H

#include <stdint.h>
#include <sys/types.h>

/* First IPv4 address of a device, or -1 (out filled with "" on error). */
int util_if_addr(const char *dev, char *out, size_t outlen);

/* Monotonic milliseconds. */
int64_t util_now_ms(void);

/* Interruptible-free sleep (nanosleep loop). */
void util_sleep_ms(int64_t ms);

/* growable string buffer for JSON/m3u builders */
struct sbuf {
	char *b;
	size_t len, cap;
};

int  sb_init(struct sbuf *sb, size_t cap);
/* returns 0 on success; on failure the buffer is left empty */
int  sb_printf(struct sbuf *sb, const char *fmt, ...);
/* append a JSON string literal (quotes + escapes); falls back to "" */
int  sb_json_str(struct sbuf *sb, const char *s);
void sb_free(struct sbuf *sb);

#endif
