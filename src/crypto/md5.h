#ifndef IPTV_MD5_H
#define IPTV_MD5_H

#include <stdint.h>
#include <stddef.h>

/* MD5 (RFC 1321), self-contained: used for the authenticator key
 * derivation (MD5("123456")).  Bundled to avoid libmbedtls ABI churn
 * between OpenWrt 19.07 and 25.12. */
void iptv_md5(const void *data, size_t len, unsigned char out[16]);

#endif
