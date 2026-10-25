#ifndef IPTV_LOG_H
#define IPTV_LOG_H

void log_init(const char *ident);
void log_level(int lvl);
void log_msg(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
