#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <time.h>

#include "log.h"
#include "iptv.h"

static int g_level = LL_INFO;

void log_init(const char *ident)
{
	openlog(ident, LOG_PID | LOG_CONS, LOG_DAEMON);
}

void log_level(int lvl)
{
	if (lvl >= LL_ERR && lvl <= LL_DEBUG)
		g_level = lvl;
}

void log_msg(int lvl, const char *fmt, ...)
{
	if (lvl > g_level)
		return;
	va_list ap;
	char buf[1024];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	int pri = (lvl == LL_ERR) ? LOG_ERR : (lvl == LL_WARN) ? LOG_WARNING
		: (lvl == LL_INFO) ? LOG_INFO : LOG_DEBUG;
	syslog(pri, "%s", buf);
	/* procd captures stdout/stderr; mirror there too */
	fprintf(lvl <= LL_WARN ? stderr : stdout, "%s\n", buf);
}
