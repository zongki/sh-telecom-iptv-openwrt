/* See xcode.h.  The child is ffmpeg with:
 *   -c:v copy                      4K HEVC re-encode is not viable
 *   -c:a aac -b:a 256k -ac 2       AC-3 -> stereo AAC; the 5.1 source
 *                                  must be down-mixed: mpegts.js
 *                                  advertises the result as HE-AAC and
 *                                  Chrome's HE-AAC decoder rejects 5.1,
 *                                  failing the whole MediaSource session
 *                                  (python reference finding, verified
 *                                  on the Sichuan 4K path)
 *   -probesize/-analyzeduration    1 MB / 1 s probe hints keep the
 *                                  zap-in delay well under a second on
 *                                  a ~20 Mbps feed
 * The exec status is reported through a CLOEXEC pipe: EOF = execved,
 * one byte = errno, so a vanished binary degrades to the raw stream
 * instead of a dead response. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "xcode.h"
#include "iptv.h"
#include "log.h"

static char *const ffmpeg_argv[] = {
	"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
	"-f", "mpegts", "-probesize", "1000000", "-analyzeduration", "1000000",
	"-i", "pipe:0",
	"-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-ac", "2",
	"-muxdelay", "0.1", "-f", "mpegts", "pipe:1",
	NULL
};

struct xcode_sess {
	struct xcode_src src;
	pid_t pid;
	int in_fd;                /* pump -> ffmpeg stdin; pump owns it */
	int out_fd;               /* ffmpeg stdout -> xcode_read */
	pthread_t pump;
	volatile int stop;
	int got_output;
};

static int missing_warned;

/* the pump must see EPIPE, not die from the default SIGPIPE
 * disposition; idempotent, and matches what the daemon main() sets */
static void ignore_sigpipe(void)
{
	static int done;
	if (!done) {
		done = 1;
		signal(SIGPIPE, SIG_IGN);
	}
}

/* first executable regular file named ffmpeg on PATH */
static const char *find_ffmpeg(void)
{
	static const char *fallback = "/usr/bin:/bin:/usr/sbin:/sbin";
	const char *path = getenv("PATH");
	if (!path || !*path)
		path = fallback;
	static char found[300];
	for (const char *p = path; *p; ) {
		const char *e = strchr(p, ':');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n && n < sizeof(found) - 12) {
			snprintf(found, sizeof(found), "%.*s/ffmpeg",
				 (int)n, p);
			struct stat st;
			if (stat(found, &st) == 0 && S_ISREG(st.st_mode) &&
			    access(found, X_OK) == 0)
				return found;
		}
		if (!e)
			break;
		p = e + 1;
	}
	return NULL;
}

/* feeds the source into ffmpeg's stdin until the source ends or
 * ffmpeg dies; closing stdin is the EOF that makes ffmpeg flush and
 * exit cleanly at end of stream */
static void *pump_thread(void *arg)
{
	struct xcode_sess *x = arg;
	char *buf = malloc(65536);   /* heap: musl thread stacks are 128KB */
	if (!buf)
		goto out;
	for (;;) {
		ssize_t n = x->src.read(x->src.src, buf, 65536);
		if (n < 0)
			break;          /* source ended */
		if (n == 0) {
			if (x->stop)
				break;
			usleep(20000);  /* defensive: sources block internally */
			continue;
		}
		size_t off = 0;
		while (off < (size_t)n) {
			ssize_t w = write(x->in_fd, buf + off,
					  (size_t)n - off);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				goto out_free;   /* EPIPE: ffmpeg gone */
			}
			off += (size_t)w;
		}
	}
out_free:
	free(buf);
out:
	close(x->in_fd);
	return NULL;
}

struct xcode_sess *xcode_open(const struct xcode_src *src)
{
	ignore_sigpipe();
	const char *ff = find_ffmpeg();
	if (!ff) {
		if (!missing_warned) {
			missing_warned = 1;
			log_msg(LL_WARN, "xcode: no ffmpeg on PATH; AC-3 "
				"channels served raw (install the ffmpeg "
				"package for audio)");
		}
		return NULL;
	}

	int in[2], out[2], rep[2];
	if (pipe(in) < 0 || pipe(out) < 0 || pipe(rep) < 0) {
		close(in[0]); close(in[1]);
		close(out[0]); close(out[1]);
		close(rep[0]); close(rep[1]);
		return NULL;
	}
	fcntl(rep[1], F_SETFD, FD_CLOEXEC);  /* closes on exec: EOF = ok */

	pid_t pid = fork();
	if (pid < 0) {
		close(in[0]); close(in[1]);
		close(out[0]); close(out[1]);
		close(rep[0]); close(rep[1]);
		return NULL;
	}
	if (pid == 0) {
		/* child: async-signal-safe calls only until execv */
		dup2(in[0], 0);
		dup2(out[1], 1);
		int dn = open("/dev/null", O_WRONLY);
		if (dn >= 0)
			dup2(dn, 2);
		long mf = sysconf(_SC_OPEN_MAX);
		if (mf < 8 || mf > 4096)
			mf = 4096;
		for (long fd = 3; fd < mf; fd++)
			if ((int)fd != rep[1])
				close((int)fd);  /* drop inherited sockets */
		execv(ff, ffmpeg_argv);
		int e = errno ? errno : 127;
		ssize_t w = write(rep[1], &e, 1);
		(void)w;
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	close(rep[1]);

	int e = 0;
	ssize_t rn;
	while ((rn = read(rep[0], &e, 1)) < 0 && errno == EINTR)
		;
	close(rep[0]);
	if (rn > 0) {
		close(in[1]);
		close(out[0]);
		int st;
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
			;
		log_msg(LL_WARN, "xcode: exec %s: %s", ff, strerror(e));
		return NULL;
	}

	struct xcode_sess *x = calloc(1, sizeof(*x));
	if (!x)
		goto fail;
	x->src = *src;
	x->pid = pid;
	x->in_fd = in[1];
	x->out_fd = out[0];
	if (pthread_create(&x->pump, NULL, pump_thread, x) != 0) {
		free(x);
		goto fail;
	}
	log_msg(LL_INFO, "xcode: ffmpeg started (%s)", ff);
	return x;

fail:
	kill(pid, SIGKILL);
	close(in[1]);
	close(out[0]);
	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	return NULL;
}

ssize_t xcode_read(void *cls, uint64_t pos, char *buf, size_t max)
{
	(void)pos;
	struct xcode_sess *x = cls;
	for (;;) {
		ssize_t n = read(x->out_fd, buf, max);
		if (n > 0) {
			x->got_output = 1;
			return n;
		}
		if (n == 0) {
			log_msg(x->got_output ? LL_INFO : LL_WARN,
				"xcode: ffmpeg ended%s",
				x->got_output ? "" : " without output");
			return -1;   /* ends the HTTP body; the client re-zaps */
		}
		if (errno == EINTR)
			continue;
		log_msg(LL_WARN, "xcode: read: %s", strerror(errno));
		return -1;
	}
}

void xcode_close(void *cls)
{
	struct xcode_sess *x = cls;
	if (!x)
		return;
	x->stop = 1;
	if (x->src.halt)
		x->src.halt(x->src.src);   /* unblock the pump's read */
	kill(x->pid, SIGKILL);        /* unblock the pump's write, stop
				       * the transcode now */
	pthread_join(x->pump, NULL);
	if (x->src.free)
		x->src.free(x->src.src);
	close(x->out_fd);
	int st;
	while (waitpid(x->pid, &st, 0) < 0 && errno == EINTR)
		;
	free(x);
}
