/* Host-side unit test for the RTP reorder window in src/mcast.c.
 *
 * Includes mcast.c directly so the static ro_* state machine is
 * reachable, and captures group_push output through a fake client
 * attached to the group (same ring buffer the real relay uses).
 *
 *   gcc -Isrc -o /tmp/reorder_test test/reorder_test.c src/fcc.c \
 *       src/log.c src/util.c -lpthread
 *   /tmp/reorder_test
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mcast.c"

static struct mgroup G;
static struct mcast_client C;
static unsigned char ring[1 << 20];
static int fails;

static void setup(void)
{
	memset(&G, 0, sizeof(G));
	memset(&C, 0, sizeof(C));
	pthread_mutex_init(&G.lock, NULL);
	pthread_mutex_init(&C.mu, NULL);
	pthread_cond_init(&C.cv, NULL);
	C.buf = ring;
	C.cap = sizeof(ring);
	G.clients = &C;
}

/* one marker byte per fed packet, so drain() records the push order */
static unsigned char mark(uint16_t seq)
{
	return (unsigned char)(seq ^ 0xA5);
}

static void feed(uint16_t seq)
{
	unsigned char p = mark(seq);
	ro_input(&G, seq, &p, 1);
}

static int drain(unsigned char *out)
{
	int n = 0;
	pthread_mutex_lock(&C.mu);
	while (C.len) {
		out[n++] = C.buf[C.head];
		C.head = (C.head + 1) % C.cap;
		C.len--;
	}
	pthread_mutex_unlock(&C.mu);
	return n;
}

static void expect(const char *name, const unsigned char *got, int n,
		   const uint16_t *want, int m)
{
	int ok = (n == m);
	for (int i = 0; ok && i < m; i++)
		ok = (got[i] == mark(want[i]));
	printf("%-32s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok) {
		fails++;
		printf("  want:");
		for (int i = 0; i < m; i++)
			printf(" %u", want[i]);
		printf("\n  got: ");
		for (int i = 0; i < n; i++)
			printf(" %u", (unsigned char)(got[i] ^ 0xA5));
		printf("\n");
	}
}

int main(void)
{
	/* case 7 drains a 1-byte packet + a full RO_MTU+1 oversize packet */
	unsigned char out[2048];
	int n;

	/* 1. in-order stream: straight through, nothing held */
	setup();
	feed(100); feed(101); feed(102);
	n = drain(out);
	expect("in-order passthrough", out, n, (uint16_t[]){100, 101, 102}, 3);

	/* 2. swapped pair: 202 held until 201 fills the gap */
	setup();
	feed(200); feed(202);
	n = drain(out);
	expect("gap holds later packet", out, n, (uint16_t[]){200}, 1);
	feed(201);
	n = drain(out);
	expect("gap fill releases in order", out, n, (uint16_t[]){201, 202}, 2);

	/* 3. duplicate of an already-pushed seq is dropped */
	setup();
	feed(30); feed(30); feed(31);
	n = drain(out);
	expect("duplicate dropped", out, n, (uint16_t[]){30, 31}, 2);

	/* 3b. duplicate of a *held* seq does not double-push */
	setup();
	feed(80); feed(82); feed(82); feed(81);
	n = drain(out);
	expect("held duplicate overwrites", out, n, (uint16_t[]){80, 81, 82}, 3);

	/* 4. gap beyond the window: held flushed, resync on the far packet */
	setup();
	feed(40); feed(140);
	n = drain(out);
	expect("oversized gap resyncs", out, n, (uint16_t[]){40, 140}, 2);
	feed(141);
	n = drain(out);
	expect("resync then in-order", out, n, (uint16_t[]){141}, 1);

	/* 5. seq wraparound */
	setup();
	feed(65534); feed(65535); feed(1); feed(0);
	n = drain(out);
	expect("wraparound", out, n, (uint16_t[]){65534, 65535, 0, 1}, 4);

	/* 6. hold timeout: missing seq skipped, late arrival dropped */
	setup();
	feed(60); feed(63);
	n = drain(out);
	expect("pre-timeout hold", out, n, (uint16_t[]){60}, 1);
	usleep(60000);                    /* RO_HOLD_MS is 40 */
	ro_tick(&G);
	n = drain(out);
	expect("timeout flush skips gap", out, n, (uint16_t[]){63}, 1);
	feed(61);
	n = drain(out);
	expect("late packet after flush dropped", out, n, (uint16_t[]){}, 0);

	/* 7. oversized packet passes through and flushes the window */
	setup();
	unsigned char big[RO_MTU + 1];
	memset(big, 0x5A, sizeof(big));
	feed(70);                          /* 71 gap start */
	big[0] = mark(71);
	ro_input(&G, 71, big, sizeof(big));   /* cannot be held: flush+push */
	n = drain(out);
	if (n != 1 + (int)sizeof(big)) {
		printf("%-32s FAIL (len %d)\n", "oversize passthrough len", n);
		fails++;
	} else if (out[0] != mark(70) || out[1] != mark(71)) {
		printf("%-32s FAIL (order)\n", "oversize passthrough order");
		fails++;
	} else {
		printf("%-32s PASS\n", "oversize passthrough");
	}
	feed(72);
	n = drain(out);
	expect("oversize then in-order", out, n, (uint16_t[]){72}, 1);

	/* 8. FCC merge overshoot vs divergent seq (r15): seqs slightly
	 *    behind ro_next are the multicast catching up with the push
	 *    - held, never resynced (the backward jump froze players);
	 *    a seq grossly behind is a different seq space - resync */
	setup();
	feed(40); feed(500);
	n = drain(out);
	expect("fwd gap resyncs", out, n, (uint16_t[]){40, 500}, 2);
	feed(490);
	n = drain(out);
	expect("overshoot held", out, n, (uint16_t[]){}, 0);
	feed(501);
	n = drain(out);
	expect("caught up flows", out, n, (uint16_t[]){501}, 1);
	feed((uint16_t)(501 - 9000));
	n = drain(out);
	expect("divergent resyncs (r15)", out, n,
	       (uint16_t[]){ (uint16_t)(501 - 9000) }, 1);
	feed((uint16_t)(502 - 9000));
	n = drain(out);
	expect("divergent then in-order", out, n,
	       (uint16_t[]){ (uint16_t)(502 - 9000) }, 1);

	printf("%s (%d failure(s))\n", fails ? "TESTS FAILED" : "all tests passed", fails);
	return fails ? 1 : 0;
}
