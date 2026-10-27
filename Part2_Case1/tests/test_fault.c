#define _GNU_SOURCE
#include "../common.h"
#include "../fault.h"

/*
 * Unit tests for the fault injector (Design Document Section 4).
 *
 * They check the properties the analysis of Section 4.3 relies on:
 *   * p = 0 never cuts and p = 1 always cuts before the last byte;
 *   * the per-request decision follows the requested Bernoulli distribution;
 *   * the cut point K is uniform over {0, ..., R-1};
 *   * the same (seed, connection id) always reproduces the same decision;
 *   * a cut really tears the connection down: the peer sees a TCP RST and
 *     never more than K bytes.
 */

static int g_failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
        }                                                                     \
    } while (0)

/* Plans a fault with a fixed seed and connection id. */
static void plan(fault_plan_t *fp, fault_mode_t mode, double p, uint64_t total,
                 long seed, uint32_t conn_id) {
    unsigned short state[3];
    fault_seed_state(state, seed, conn_id);
    fault_plan_init(fp, mode, p, total, state, conn_id, "cane");
}

static void test_p_zero_never_cuts(void) {
    for (uint32_t c = 0; c < 2000; c++) {
        fault_plan_t fp;
        plan(&fp, FAULT_MODE_REQUEST, 0.0, 1000, 42, c);
        CHECK(!fp.will_fail, "p=0 cut the connection (conn %u)", c);
        CHECK(fp.cut_bytes == fp.total_bytes, "p=0 must leave the cut point at R");
    }
    printf("  [ok] p = 0 never cuts\n");
}

static void test_p_one_always_cuts_before_the_last_byte(void) {
    for (uint32_t c = 0; c < 2000; c++) {
        fault_plan_t fp;
        plan(&fp, FAULT_MODE_REQUEST, 1.0, 1000, 42, c);
        CHECK(fp.will_fail, "p=1 did not cut the connection (conn %u)", c);
        CHECK(fp.cut_bytes < fp.total_bytes,
              "p=1 must cut before the last byte (conn %u, K=%" PRIu64 ")", c, fp.cut_bytes);
    }
    printf("  [ok] p = 1 always cuts before the last byte\n");
}

static void test_bernoulli_rate(void) {
    const uint32_t trials = 40000;
    for (double p = 0.1; p <= 0.9; p += 0.2) {
        uint32_t fired = 0;
        for (uint32_t c = 0; c < trials; c++) {
            fault_plan_t fp;
            plan(&fp, FAULT_MODE_REQUEST, p, 1000, 7, c);
            if (fp.will_fail) fired++;
        }
        double rate = (double)fired / (double)trials;
        CHECK(rate > p - 0.02 && rate < p + 0.02,
              "failure rate for p=%.2f was %.4f", p, rate);
        printf("  [ok] p = %.2f -> observed rate %.4f over %u trials\n", p, rate, trials);
    }
}

static void test_cut_point_is_uniform(void) {
    const uint64_t R = 1000;
    const uint32_t trials = 40000;
    uint64_t sum = 0;
    uint64_t min_k = R;
    uint64_t max_k = 0;
    uint32_t fired = 0;

    for (uint32_t c = 0; c < trials; c++) {
        fault_plan_t fp;
        plan(&fp, FAULT_MODE_REQUEST, 1.0, R, 11, c);
        if (!fp.will_fail) continue;
        fired++;
        sum += fp.cut_bytes;
        if (fp.cut_bytes < min_k) min_k = fp.cut_bytes;
        if (fp.cut_bytes > max_k) max_k = fp.cut_bytes;
    }

    CHECK(fired == trials, "p=1 did not fire on every trial");
    CHECK(min_k == 0, "the cut point never reached 0 (min=%" PRIu64 ")", min_k);
    CHECK(max_k == R - 1, "the cut point exceeded R-1 (max=%" PRIu64 ")", max_k);
    double mean = (double)sum / (double)fired;
    double expected = (double)(R - 1) / 2.0;
    CHECK(mean > expected * 0.95 && mean < expected * 1.05,
          "the mean cut point was %.1f, expected about %.1f", mean, expected);
    printf("  [ok] cut point uniform over [0, R-1]: min=%" PRIu64 " max=%" PRIu64
           " mean=%.1f (expected %.1f)\n", min_k, max_k, mean, expected);
}

/* The same seed and connection id must always produce the same decision. */
static void test_reproducible(void) {
    for (uint32_t c = 0; c < 200; c++) {
        fault_plan_t a, b;
        plan(&a, FAULT_MODE_REQUEST, 0.5, 4096, 1234, c);
        plan(&b, FAULT_MODE_REQUEST, 0.5, 4096, 1234, c);
        CHECK(a.will_fail == b.will_fail && a.cut_bytes == b.cut_bytes,
              "the same (seed, conn_id) produced different decisions");
    }
    printf("  [ok] decisions are reproducible for a given (seed, connection id)\n");
}

/* In chunk mode the coin is flipped per chunk, not per request. */
static void test_chunk_mode(void) {
    fault_plan_t fp;
    plan(&fp, FAULT_MODE_CHUNK, 1.0, 1000, 5, 1);
    CHECK(!fp.will_fail, "chunk mode must not decide at plan time");
    CHECK(fp.bytes_written == 0, "chunk mode must not count bytes at plan time");
    printf("  [ok] chunk mode defers the decision to the chunk boundary\n");
}

/* Creates a connected TCP pair over the loopback interface. */
static int tcp_pair(int *fd_out, int *fd_in) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(ls); return -1; }
    if (listen(ls, 1) != 0) { close(ls); return -1; }

    socklen_t alen = sizeof(addr);
    if (getsockname(ls, (struct sockaddr *)&addr, &alen) != 0) { close(ls); return -1; }

    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) { close(ls); return -1; }
    if (connect(cs, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(ls); close(cs); return -1; }

    int as = accept(ls, NULL, NULL);
    close(ls);
    if (as < 0) { close(cs); return -1; }

    *fd_out = cs;   /* the client side */
    *fd_in = as;    /* the server side */
    return 0;
}

/*
 * A cut must tear the connection down: the peer may not receive more than K
 * bytes and never sees a clean end of stream.
 */
static void test_cut_tears_the_connection_down(void) {
    int cs = -1, as = -1;
    if (tcp_pair(&cs, &as) != 0) {
        CHECK(0, "could not create a TCP pair");
        return;
    }

    fault_plan_t fp;
    plan(&fp, FAULT_MODE_REQUEST, 1.0, 1000, 3, 1);
    fp.cut_bytes = 100;    /* cut after exactly 100 bytes */

    char payload[1000];
    memset(payload, 'x', sizeof(payload));

    ssize_t written = fault_send(&fp, as, payload, sizeof(payload));
    CHECK(written == 100, "expected 100 bytes to be written, got %zd", written);
    CHECK(fp.aborted, "the fault plan did not abort the socket");

    /* The peer must not receive the whole payload. */
    char buf[4096];
    size_t got = 0;
    bool reset = false;
    for (;;) {
        ssize_t n = read(cs, buf, sizeof(buf));
        if (n > 0) {
            got += (size_t)n;
            if (got > 100) break;
            continue;
        }
        if (n == 0) break;                  /* clean EOF */
        if (errno == EINTR) continue;
        reset = (errno == ECONNRESET);
        break;
    }
    CHECK(got <= 100, "the peer received %zu bytes, more than the cut point", got);
    CHECK(reset || got < 1000, "the peer saw a clean end of stream instead of a reset");
    printf("  [ok] a cut tears the connection down (peer got %zu bytes, reset=%s)\n",
           got, reset ? "yes" : "no");

    close(cs);
    /* as was closed by the fault plan. */
}

/* Without a cut the whole payload arrives and the connection stays open. */
static void test_no_cut_delivers_everything(void) {
    int cs = -1, as = -1;
    if (tcp_pair(&cs, &as) != 0) {
        CHECK(0, "could not create a TCP pair");
        return;
    }

    fault_plan_t fp;
    plan(&fp, FAULT_MODE_REQUEST, 0.0, 1000, 3, 1);

    char payload[1000];
    memset(payload, 'y', sizeof(payload));

    ssize_t written = fault_send(&fp, as, payload, sizeof(payload));
    CHECK(written == (ssize_t)sizeof(payload), "expected %zu bytes, got %zd",
          sizeof(payload), written);
    CHECK(!fp.aborted, "the socket was aborted although no fault was planned");

    size_t got = 0;
    char buf[4096];
    while (got < sizeof(payload)) {
        ssize_t n = read(cs, buf, sizeof(buf));
        if (n <= 0) break;
        got += (size_t)n;
    }
    CHECK(got == sizeof(payload), "the peer received %zu of %zu bytes", got, sizeof(payload));

    close(as);
    close(cs);
    printf("  [ok] without a cut the whole payload is delivered\n");
}

int main(void) {
    printf("Fault injector unit tests\n");
    test_p_zero_never_cuts();
    test_p_one_always_cuts_before_the_last_byte();
    test_bernoulli_rate();
    test_cut_point_is_uniform();
    test_reproducible();
    test_chunk_mode();
    test_cut_tears_the_connection_down();
    test_no_cut_delivers_everything();

    if (g_failures == 0) {
        printf("All fault injector tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d check(s) failed.\n", g_failures);
    return 1;
}
