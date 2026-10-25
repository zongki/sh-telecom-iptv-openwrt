/* MD5 message digest (RFC 1321), compact public-domain style
 * implementation.  Verification vector: md5("abc") must equal
 * 900150983cd24fb0d6963f7d28e17f72.
 */
#include <string.h>

#include "md5.h"

struct md5_ctx {
	uint32_t a, b, c, d;
	uint64_t len;
	unsigned char buf[64];
	size_t buflen;
};

static uint32_t rol(uint32_t x, int c)
{
	return (x << c) | (x >> (32 - c));
}

static void md5_block(struct md5_ctx *ctx, const unsigned char *p)
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
		0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
		0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
		0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
		0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
		0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
		0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
		0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
		0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
	};
	static const int S[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
	};
	uint32_t m[16];
	int i;
	for (i = 0; i < 16; i++)
		m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
		       ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);

	uint32_t a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
	for (i = 0; i < 64; i++) {
		uint32_t f;
		int g;
		if (i < 16)      { f = (b & c) | (~b & d);      g = i; }
		else if (i < 32) { f = (d & b) | (~d & c);      g = (5 * i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d;               g = (3 * i + 5) & 15; }
		else             { f = c ^ (b | ~d);            g = (7 * i) & 15; }
		uint32_t tmp = d;
		d = c;
		c = b;
		b = b + rol(a + f + K[i] + m[g], S[i]);
		a = tmp;
	}
	ctx->a += a; ctx->b += b; ctx->c += c; ctx->d += d;
}

void iptv_md5(const void *data, size_t len, unsigned char out[16])
{
	struct md5_ctx ctx;
	ctx.a = 0x67452301; ctx.b = 0xefcdab89;
	ctx.c = 0x98badcfe; ctx.d = 0x10325476;
	ctx.len = 0; ctx.buflen = 0;

	const unsigned char *p = data;
	while (len > 0) {
		size_t take = 64 - ctx.buflen;
		if (take > len)
			take = len;
		memcpy(ctx.buf + ctx.buflen, p, take);
		ctx.buflen += take;
		p += take;
		len -= take;
		if (ctx.buflen == 64) {
			md5_block(&ctx, ctx.buf);
			ctx.len += 64;
			ctx.buflen = 0;
		}
	}

	uint64_t total = ctx.len + ctx.buflen;
	unsigned char tail[72];
	memset(tail, 0, sizeof(tail));
	memcpy(tail, ctx.buf, ctx.buflen);
	tail[ctx.buflen] = 0x80;
	size_t padlen = (ctx.buflen < 56) ? 56 : 120;
	/* length in bits, little-endian */
	uint64_t bits = (uint64_t)total * 8;
	memcpy(tail + padlen, &bits, 8);
	md5_block(&ctx, tail);
	if (padlen == 120)
		md5_block(&ctx, tail + 64);

	uint32_t reg[4] = { ctx.a, ctx.b, ctx.c, ctx.d };
	memcpy(out, reg, 16);
}
