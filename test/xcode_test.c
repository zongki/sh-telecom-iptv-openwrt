/* xcode module host tests.
 *
 * Build:
 *   gcc -std=gnu99 -Wall -Wextra -Isrc -o /tmp/xcode_test \
 *       test/xcode_test.c src/xcode.c src/log.c -lpthread && /tmp/xcode_test
 *
 * Cases:
 *   1. no ffmpeg on PATH          -> xcode_open NULL, source untouched
 *   2. stub ffmpeg (cat)          -> byte-exact passthrough, graceful end
 *   3. stub ffmpeg dies midstream -> xcode_read returns -1, no hang
 *   4. blocked source + close     -> halt unblocks pump, free runs once
 *   5. real ffmpeg (if installed) -> AC-3 TS in, output PMT carries AAC
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>

#include "xcode.h"
#include "log.h"

static int fails;

#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok: %s\n", msg); } \
	else { printf("  FAIL: %s\n", msg); fails++; } \
} while (0)

/* -------- pattern source -------- */

struct gensrc {
	const unsigned char *data;   /* NULL = synthesize a pattern */
	size_t total, pos;
	int freed;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int block;                   /* read() waits until halted */
	int halted;
};

static void gen_init(struct gensrc *g, const unsigned char *data, size_t total)
{
	memset(g, 0, sizeof(*g));
	g->data = data;
	g->total = total;
	pthread_mutex_init(&g->mu, NULL);
	pthread_cond_init(&g->cv, NULL);
}

static ssize_t gen_read(void *p, char *buf, size_t max)
{
	struct gensrc *g = p;
	pthread_mutex_lock(&g->mu);
	while (g->block && !g->halted)
		pthread_cond_wait(&g->cv, &g->mu);
	pthread_mutex_unlock(&g->mu);
	if (g->pos >= g->total)
		return -1;
	size_t n = g->total - g->pos;
	if (n > max)
		n = max;
	for (size_t i = 0; i < n; i++)
		buf[i] = g->data ? g->data[g->pos + i]
				 : (unsigned char)((g->pos + i) * 31 + 7);
	g->pos += n;
	return (ssize_t)n;
}

static void gen_halt(void *p)
{
	struct gensrc *g = p;
	pthread_mutex_lock(&g->mu);
	g->halted = 1;
	pthread_cond_broadcast(&g->cv);
	pthread_mutex_unlock(&g->mu);
}

static void gen_free(void *p)
{
	((struct gensrc *)p)->freed++;
}

static struct xcode_src gen_src(struct gensrc *g)
{
	struct xcode_src s = { gen_read, gen_halt, gen_free, g };
	return s;
}

/* read everything xcode_read yields; returns total bytes (-1 pattern
 * check skipped when out != NULL: verify equals source bytes) */
static size_t drain(struct xcode_sess *x, unsigned char *out, size_t outcap)
{
	size_t got = 0;
	char buf[16384];
	for (;;) {
		ssize_t n = xcode_read(x, 0, buf, sizeof(buf));
		if (n < 0)
			return got;
		if (got + (size_t)n <= outcap)
			memcpy(out + got, buf, (size_t)n);
		got += (size_t)n;
	}
}

static int fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

/* -------- minimal PAT/PMT walk: does the TS carry an AAC ES? -------- */

static int ts_has_aac(const unsigned char *ts, size_t n)
{
	int pmt_pid = -1;
	size_t pkts = n / 188;
	for (size_t k = 0; k < pkts && pmt_pid < 0; k++) {
		const unsigned char *p = ts + k * 188;
		if (p[0] != 0x47 || (((p[1] & 0x1f) << 8) | p[2]) != 0)
			continue;
		const unsigned char *sec = p + 5 + p[4];
		if (sec[0] != 0x00)
			continue;
		int slen = ((sec[1] & 0x3) << 8) | sec[2];
		for (int off = 8; off + 4 <= slen && off - 8 < 184 - 8; off += 4) {
			int prog = (sec[off] << 8) | sec[off + 1];
			int pid = ((sec[off + 2] & 0x1f) << 8) | sec[off + 3];
			if (prog != 0) {
				pmt_pid = pid;
				break;
			}
		}
	}
	if (pmt_pid < 0)
		return -1;
	for (size_t k = 0; k < pkts; k++) {
		const unsigned char *p = ts + k * 188;
		if (p[0] != 0x47 || (((p[1] & 0x1f) << 8) | p[2]) != pmt_pid)
			continue;
		const unsigned char *sec = p + 5 + p[4];
		if (sec[0] != 0x02)
			continue;
		int slen = ((sec[1] & 0x3) << 8) | sec[2];
		int pinfo = ((sec[10] & 0x0f) << 8) | sec[11];
		int off = 12 + pinfo;
		while (off + 5 <= slen) {          /* ES loop, CRC32 excluded */
			int stype = sec[off];
			int elen = ((sec[off + 3] & 0x0f) << 8) | sec[off + 4];
			if (stype == 0x0f)             /* AAC ADTS */
				return 1;
			off += 5 + elen;
		}
	}
	return 0;
}

/* -------- cases -------- */

static void write_stub(const char *dir, const char *body)
{
	char path[256];
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/ffmpeg", dir);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(body, f);
		fclose(f);
	}
	chmod(path, 0755);
}

int main(void)
{
	log_level(1);   /* quiet: only warns from the module */
	char dir[192], empty[224];
	snprintf(dir, sizeof(dir), "/tmp/xcode_t%d", (int)getpid());
	snprintf(empty, sizeof(empty), "%s/empty", dir);
	/* stub bodies use absolute commands: PATH is redirected around the
	 * stub dir, so a bare "cat"/"head" would not be found (sh exits 127
	 * and the stream ends empty) */
	write_stub(dir, "#!/bin/sh\nexec /bin/cat\n");
	mkdir(empty, 0755);

	int fds0 = fd_count();

	/* 1: no ffmpeg anywhere on PATH */
	printf("case 1: no ffmpeg\n");
	{
		char oldpath[4096];
		snprintf(oldpath, sizeof(oldpath), "%s", getenv("PATH") ? getenv("PATH") : "");
		setenv("PATH", empty, 1);
		struct gensrc g;
		gen_init(&g, NULL, 100000);
		struct xcode_src s = gen_src(&g);
		struct xcode_sess *x = xcode_open(&s);
		CHECK(x == NULL, "open returns NULL without ffmpeg");
		CHECK(g.pos == 0, "source not consumed");
		CHECK(g.freed == 0, "source not freed (caller owns it)");
		setenv("PATH", oldpath, 1);
	}

	/* 2: cat stub = byte-exact passthrough */
	printf("case 2: passthrough stub\n");
	{
		char oldpath[4096];
		snprintf(oldpath, sizeof(oldpath), "%s", getenv("PATH") ? getenv("PATH") : "");
		setenv("PATH", dir, 1);
		struct gensrc g;
		gen_init(&g, NULL, 300000);
		struct xcode_src s = gen_src(&g);
		struct xcode_sess *x = xcode_open(&s);
		CHECK(x != NULL, "open with stub ffmpeg");
		if (x) {
			unsigned char *out = malloc(300000);
			size_t n = drain(x, out, 300000);
			CHECK(n == 300000, "all bytes came through");
			int same = 1;
			for (size_t i = 0; i < n; i++)
				if (out[i] != (unsigned char)(i * 31 + 7))
					same = 0;
			CHECK(same, "byte-exact content");
			free(out);
			xcode_close(x);
			CHECK(g.freed == 1, "source freed exactly once");
		}
		setenv("PATH", oldpath, 1);
	}

	/* 3: stub dies after 64 KiB -> graceful -1, no hang */
	printf("case 3: ffmpeg dies midstream\n");
	{
		char die_dir[224], oldpath[4096];
		snprintf(die_dir, sizeof(die_dir), "%s/die", dir);
		write_stub(die_dir, "#!/bin/sh\nexec /usr/bin/head -c 65536\n");
		snprintf(oldpath, sizeof(oldpath), "%s", getenv("PATH") ? getenv("PATH") : "");
		setenv("PATH", die_dir, 1);
		struct gensrc g;
		gen_init(&g, NULL, 400000);
		struct xcode_src s = gen_src(&g);
		struct xcode_sess *x = xcode_open(&s);
		CHECK(x != NULL, "open with dying stub");
		if (x) {
			size_t n = drain(x, NULL, 0);
			CHECK(n == 65536, "exactly the stub's output, then -1");
			xcode_close(x);
			CHECK(g.freed == 1, "source freed after die");
		}
		setenv("PATH", oldpath, 1);
	}

	/* 4: blocked source: close must halt the pump, not deadlock */
	printf("case 4: blocked source close\n");
	{
		char oldpath[4096];
		snprintf(oldpath, sizeof(oldpath), "%s", getenv("PATH") ? getenv("PATH") : "");
		setenv("PATH", dir, 1);   /* cat stub */
		struct gensrc g;
		gen_init(&g, NULL, 100000);
		g.block = 1;              /* read() parks until halt */
		struct xcode_src s = gen_src(&g);
		struct xcode_sess *x = xcode_open(&s);
		CHECK(x != NULL, "open with blocked source");
		if (x) {
			struct timespec a, b;
			clock_gettime(CLOCK_MONOTONIC, &a);
			xcode_close(x);
			clock_gettime(CLOCK_MONOTONIC, &b);
			double ms = (b.tv_sec - a.tv_sec) * 1000.0 +
				    (b.tv_nsec - a.tv_nsec) / 1e6;
			CHECK(ms < 5000, "close returned promptly");
			printf("    (close took %.0f ms)\n", ms);
			CHECK(g.freed == 1, "source freed despite blocked read");
		}
		setenv("PATH", oldpath, 1);
	}

	/* 5: real ffmpeg: AC-3 in -> AAC out */
	printf("case 5: real transcode (AC-3 -> AAC)\n");
	if (access("/usr/bin/ffmpeg", X_OK) == 0) {
		char inpath[224], cmd[512];
		snprintf(inpath, sizeof(inpath), "%s/in.ts", dir);
		snprintf(cmd, sizeof(cmd),
			 "ffmpeg -hide_banner -loglevel error -y "
			 "-f lavfi -i testsrc2=size=320x240:rate=25 "
			 "-f lavfi -i sine=frequency=440:sample_rate=48000 "
			 "-t 5 -c:v mpeg2video -b:v 400k "
			 "-c:a ac3 -b:a 192k -f mpegts %s", inpath);
		int rc = system(cmd);
		CHECK(rc == 0, "generated AC-3 test TS");
		if (rc == 0) {
			FILE *f = fopen(inpath, "rb");
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			unsigned char *in = malloc(sz);
			if (fread(in, 1, sz, f) != (size_t)sz)
				sz = 0;
			fclose(f);
			char oldpath[4096];
			snprintf(oldpath, sizeof(oldpath), "%s", getenv("PATH") ? getenv("PATH") : "");
			setenv("PATH", "/usr/bin:/bin", 1);
			struct gensrc g;
			gen_init(&g, in, sz);
			struct xcode_src s = gen_src(&g);
			struct xcode_sess *x = xcode_open(&s);
			CHECK(x != NULL, "open with real ffmpeg");
			if (x) {
				unsigned char *out = malloc(4 * 1024 * 1024);
				size_t n = drain(x, out, 4 * 1024 * 1024);
				CHECK(n > 100000, "substantial output");
				CHECK(n % 188 == 0 || n == 0, "188-aligned output length");
				int aac = n ? ts_has_aac(out, n) : -1;
				CHECK(aac == 1, "output PMT carries an AAC ES");
				printf("    (%ld B in, %zu B out, aac=%d)\n",
				       sz, n, aac);
				free(out);
				xcode_close(x);
				CHECK(g.freed == 1, "source freed after e2e");
			}
			setenv("PATH", oldpath, 1);
			free(in);
		}
	} else {
		printf("  skipped: no /usr/bin/ffmpeg on host\n");
	}

	CHECK(fd_count() == fds0, "no fd leaks");

	char rm[224];
	snprintf(rm, sizeof(rm), "rm -rf %s", dir);
	system(rm);

	if (fails) {
		printf("%d FAILURE(S)\n", fails);
		return 1;
	}
	printf("all ok\n");
	return 0;
}
