#ifndef IPTV_HTTPD_H
#define IPTV_HTTPD_H

#include "iptv.h"

#define IPTVD_VERSION "1.1.0"

int  httpd_run(struct iptv_cfg *cfg);
void httpd_stop(void);

#endif
