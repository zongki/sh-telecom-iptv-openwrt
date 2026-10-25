#ifndef IPTV_AES_H
#define IPTV_AES_H

#include <stdint.h>
#include <stddef.h>

/* AES-128 ECB, encryption only, PKCS#7 padded - exactly what the STB
 * authenticator needs (JSON -> AES-128-ECB(key=MD5("123456")) -> hex).
 * Bundled (not libmbedtls) so the package links identically on
 * OpenWrt 19.07 and 25.12. */
size_t iptv_aes128_ecb_encrypt_pkcs7(const unsigned char key[16],
				     const void *plain, size_t len,
				     unsigned char *out, size_t outcap);

#endif
