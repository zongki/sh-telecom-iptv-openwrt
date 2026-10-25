#ifndef IPTV_XCODE_H
#define IPTV_XCODE_H

#include <stdint.h>
#include <stddef.h>

/* 1.1: AC-3 -> AAC audio transcode wrapper (python xcode.aac_restream).
 *
 * The 4K channels are HEVC + AC-3, and browser MSE has no AC-3 decode
 * license: the audio track never enters the MediaSource, so the picture
 * plays but there is no sound and the volume control does nothing (the
 * Olympic 4K channel 9999, whose audio is not AC-3, works natively and
 * is the counter-example that pinned this).  Piping the raw TS through
 * ffmpeg with the video copied and the audio re-encoded to stereo AAC
 * makes those channels fully playable; audio-only encoding is cheap
 * even on the WF825.  One ffmpeg child per stream, like the python
 * reference.  When ffmpeg is not installed the wrapper is not created
 * and the raw stream is served exactly as before. */

struct xcode_sess;

struct xcode_src {
	/* source reader, MHD streaming semantics: >0 bytes copied,
	 * 0 = nothing yet (will be called again), -1 = stream ended */
	ssize_t (*read)(void *src, char *buf, size_t max);
	/* make a concurrently blocked read() return promptly; must not
	 * free the source (free below still runs, once, afterwards) */
	void (*halt)(void *src);
	/* final release, called once after the pump thread exited */
	void (*free)(void *src);
	void *src;
};

/* Spawn ffmpeg for this source.  NULL = ffmpeg unavailable (the caller
 * serves the source directly); on success the session owns src - its
 * halt/free are called from xcode_close. */
struct xcode_sess *xcode_open(const struct xcode_src *src);

/* Both have the MHD content-reader shapes, so they can be handed to
 * MHD_create_response_from_callback as-is.  xcode_read returns the
 * transcoded bytes, -1 when ffmpeg ended (the HTTP body ends with it);
 * xcode_close tears down child + pump and releases the source. */
ssize_t xcode_read(void *cls, uint64_t pos, char *buf, size_t max);
void xcode_close(void *cls);

#endif
