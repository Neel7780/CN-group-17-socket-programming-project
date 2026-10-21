#define _GNU_SOURCE
#include "../common.h"
#include "../proto.h"
#include "../metrics.h"

/*
 * Unit tests for the buffered line reader, the Part I protocol parsers, and the
 * metrics logger. Run with `make test`.
 */

static int g_failures = 0;

#define CHECK(cond) do {                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* Returns the read end of a pipe pre-loaded with data (writer closed => EOF after data). */
static int pipe_with(const void *data, size_t len) {
    int p[2];
    if (pipe(p) != 0) abort();
    if (write_all(p[1], data, len) != (ssize_t)len) abort();
    close(p[1]);
    return p[0];
}

typedef struct {
    int fd;
    const char *data;
    size_t len;
} dribble_arg_t;

/* Writes one byte at a time so every read() returns a fragment. */
static void *dribble_writer(void *arg) {
    dribble_arg_t *a = arg;
    for (size_t i = 0; i < a->len; i++) {
        write_all(a->fd, a->data + i, 1);
        if (i % 7 == 0) usleep(100);
    }
    close(a->fd);
    return NULL;
}

static void test_header_and_payload_in_one_segment(void) {
    const char msg[] = "OK 2 8\nFILE 5 a b.jpg\nHELLOFILE 3 c\nxyzEND 2 8\n";
    int fd = pipe_with(msg, sizeof(msg) - 1);
    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE], payload[16];
    CHECK(lr_readline(&lr, line, sizeof(line)) == 7);
    CHECK(strcmp(line, "OK 2 8") == 0);
    CHECK(lr_readline(&lr, line, sizeof(line)) == 15);
    CHECK(strcmp(line, "FILE 5 a b.jpg") == 0);
    CHECK(lr_read_exact(&lr, payload, 5) == 5);
    CHECK(memcmp(payload, "HELLO", 5) == 0);
    CHECK(lr_readline(&lr, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "FILE 3 c") == 0);
    CHECK(lr_read_exact(&lr, payload, 3) == 3);
    CHECK(memcmp(payload, "xyz", 3) == 0);
    CHECK(lr_readline(&lr, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "END 2 8") == 0);
    CHECK(lr_readline(&lr, line, sizeof(line)) == 0);
    CHECK(lr.bytes_in == sizeof(msg) - 1);
    close(fd);
}

static void test_fragmented_stream(void) {
    /* Binary payload containing '\n' and '\0' must pass through untouched */
    static const char msg[] = "FILE 6 x\nA\nB\0C\nEND 1 6\r\n";
    int p[2];
    if (pipe(p) != 0) abort();
    dribble_arg_t arg = { p[1], msg, sizeof(msg) - 1 };
    pthread_t t;
    pthread_create(&t, NULL, dribble_writer, &arg);

    line_reader_t lr;
    lr_init(&lr, p[0]);
    char line[MAX_LINE], payload[8];
    CHECK(lr_readline(&lr, line, sizeof(line)) == 9);
    CHECK(strcmp(line, "FILE 6 x") == 0);
    CHECK(lr_read_exact(&lr, payload, 6) == 6);
    CHECK(memcmp(payload, "A\nB\0C\n", 6) == 0);
    CHECK(lr_readline(&lr, line, sizeof(line)) == 9);
    CHECK(strcmp(line, "END 1 6") == 0);  /* CRLF tolerated */
    CHECK(lr_readline(&lr, line, sizeof(line)) == 0);

    pthread_join(t, NULL);
    close(p[0]);
}

static void test_large_payload_bypasses_buffer(void) {
    size_t n = LR_BUF_SIZE * 3 + 123;
    char *msg = malloc(n + 16);
    int hl = sprintf(msg, "FILE %zu z\n", n);
    for (size_t i = 0; i < n; i++) msg[hl + (int)i] = (char)(i * 31);
    int p[2];
    if (pipe(p) != 0) abort();
    dribble_arg_t arg = { p[1], msg, (size_t)hl + n };
    pthread_t t;
    pthread_create(&t, NULL, dribble_writer, &arg);

    line_reader_t lr;
    lr_init(&lr, p[0]);
    char line[MAX_LINE];
    char *out = malloc(n);
    CHECK(lr_readline(&lr, line, sizeof(line)) == hl);
    CHECK(lr_read_exact(&lr, out, n) == (ssize_t)n);
    CHECK(memcmp(out, msg + hl, n) == 0);
    CHECK(lr.bytes_in == (uint64_t)hl + n);

    pthread_join(t, NULL);
    close(p[0]);
    free(out);
    free(msg);
}

static void test_edge_cases(void) {
    char line[16];
    line_reader_t lr;

    /* Empty line is a line, not EOF */
    int fd = pipe_with("\n", 1);
    lr_init(&lr, fd);
    CHECK(lr_readline(&lr, line, sizeof(line)) == 1);
    CHECK(line[0] == '\0');
    CHECK(lr_readline(&lr, line, sizeof(line)) == 0);
    close(fd);

    /* Line longer than the caller's buffer is an error, not a silent split */
    fd = pipe_with("0123456789abcdefXYZ\nnext\n", 25);
    lr_init(&lr, fd);
    errno = 0;
    CHECK(lr_readline(&lr, line, sizeof(line)) == -1);
    CHECK(errno == EMSGSIZE);
    close(fd);

    /* A line exactly filling the buffer (15 chars + NUL) still fits */
    fd = pipe_with("0123456789abcde\n", 16);
    lr_init(&lr, fd);
    CHECK(lr_readline(&lr, line, sizeof(line)) == 16);
    CHECK(strcmp(line, "0123456789abcde") == 0);
    close(fd);

    /* EOF mid-line */
    fd = pipe_with("END 1", 5);
    lr_init(&lr, fd);
    errno = 0;
    CHECK(lr_readline(&lr, line, sizeof(line)) == -1);
    CHECK(errno == EPROTO);
    close(fd);

    /* Short payload reports how much arrived */
    fd = pipe_with("abc", 3);
    lr_init(&lr, fd);
    char buf[8];
    CHECK(lr_read_exact(&lr, buf, 8) == 3);
    close(fd);
}

static void test_proto(void) {
    uint32_t count;
    uint64_t total, size;
    char name[MAX_NAME_LEN + 1];
    int code;
    const char *reason;

    CHECK(proto_parse_ok("OK 4863 125829120", &count, &total) == 0);
    CHECK(count == 4863 && total == 125829120ULL);
    CHECK(proto_parse_ok("OK 18446744073709551615 1", &count, &total) == -1);  /* count > u32 */
    CHECK(proto_parse_ok("OK 1 18446744073709551615", &count, &total) == 0);
    CHECK(total == UINT64_MAX);
    CHECK(proto_parse_ok("OK 1 18446744073709551616", &count, &total) == -1); /* overflow */
    CHECK(proto_parse_ok("OK -1 5", &count, &total) == -1);
    CHECK(proto_parse_ok("OK 1 5 extra", &count, &total) == -1);
    CHECK(proto_parse_ok("OK  1 5", &count, &total) == -1);
    CHECK(proto_parse_ok("OKAY 1 5", &count, &total) == -1);
    CHECK(proto_parse_end("END 2 8", &count, &total) == 0 && count == 2 && total == 8);
    CHECK(proto_parse_end("OK 2 8", &count, &total) == -1);

    CHECK(proto_parse_file_hdr("FILE 25600 dog 0001.jpg", &size, name, sizeof(name)) == 0);
    CHECK(size == 25600 && strcmp(name, "dog 0001.jpg") == 0);
    CHECK(proto_parse_file_hdr("FILE 0 empty", &size, name, sizeof(name)) == 0 && size == 0);
    CHECK(proto_parse_file_hdr("FILE 10 ", &size, name, sizeof(name)) == -1);
    CHECK(proto_parse_file_hdr("FILE x a", &size, name, sizeof(name)) == -1);
    CHECK(proto_parse_file_hdr("FILE 10", &size, name, sizeof(name)) == -1);

    CHECK(proto_parse_err("ERR 404 NO SUCH TOPIC", &code, &reason) == 0);
    CHECK(code == 404 && strcmp(reason, "NO SUCH TOPIC") == 0);
    CHECK(proto_parse_err("ERR 400", &code, &reason) == 0 && code == 400 && *reason == '\0');
    CHECK(proto_parse_err("ERROR 404 x", &code, &reason) == -1);
    CHECK(proto_parse_err("OK 1 2", &code, &reason) == -1);

    proto_request_t req;
    CHECK(proto_parse_request("GET cane", &req) == 0 && req.type == CMD_GET);
    CHECK(strcmp(req.topic, "cane") == 0);
    CHECK(proto_parse_request("GET ../etc", &req) == -1);
    CHECK(proto_parse_request("GET a/b", &req) == -1);
    CHECK(proto_parse_request("GET cane extra", &req) == -1);
    CHECK(proto_parse_request("GET ", &req) == -1);
    CHECK(proto_parse_request("PUT cane", &req) == -1 && req.type == CMD_UNKNOWN);

    CHECK(proto_valid_topic("Scoiattolo"));
    CHECK(!proto_valid_topic(""));
    CHECK(!proto_valid_topic("a\tb"));
    char long_topic[MAX_TOPIC_LEN + 2];
    memset(long_topic, 'x', sizeof(long_topic) - 1);
    long_topic[sizeof(long_topic) - 1] = '\0';
    CHECK(!proto_valid_topic(long_topic));
    long_topic[MAX_TOPIC_LEN] = '\0';
    CHECK(proto_valid_topic(long_topic));

    CHECK(proto_safe_filename("OIP-abc.jpeg"));
    CHECK(proto_safe_filename("with space.jpg"));
    CHECK(!proto_safe_filename(".."));
    CHECK(!proto_safe_filename("../x"));
    CHECK(!proto_safe_filename("a/b"));
    CHECK(!proto_safe_filename(""));

    /* Formatters round-trip through the parsers */
    char buf[MAX_LINE];
    CHECK(proto_fmt_ok(buf, sizeof(buf), 3, 99) == 8 && strcmp(buf, "OK 3 99\n") == 0);
    CHECK(proto_fmt_file_hdr(buf, sizeof(buf), 7, "a b") > 0);
    buf[strlen(buf) - 1] = '\0';
    CHECK(proto_parse_file_hdr(buf, &size, name, sizeof(name)) == 0 && size == 7 && strcmp(name, "a b") == 0);
    CHECK(proto_fmt_end(buf, sizeof(buf), 3, 99) > 0 && strcmp(buf, "END 3 99\n") == 0);
    CHECK(proto_fmt_err(buf, sizeof(buf), 404, "NO SUCH TOPIC") > 0);
    CHECK(strcmp(buf, "ERR 404 NO SUCH TOPIC\n") == 0);
    CHECK(proto_fmt_ok(buf, 4, 3, 99) == -1);
}

static int count_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!needle || strstr(line, needle)) n++;
    }
    fclose(f);
    return n;
}

static void test_metrics(metrics_mode_t mode) {
    char path[] = "/tmp/metrics_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    for (int round = 0; round < 2; round++) {
        CHECK(metrics_open(path, mode) == 0);
        for (int i = 0; i < 50; i++) {
            metrics_row(&(metrics_row_t){
                .role = "server", .mode = "multi", .conn_id = (uint64_t)i, .topic = "a,\"b\"",
                .files = 2, .payload_bytes = 1000000, .wire_bytes = 1000040,
                .wall_start_ns = 1000, .wait_ns = 0, .duration_ns = 1000000000ULL, .status = "OK",
            });
        }
        metrics_close();
    }

    CHECK(count_lines(path, NULL) == 101);  /* Header written once across reopen */
    CHECK(count_lines(path, "wall_start_ns,") == 1);
    CHECK(count_lines(path, ",\"a,\"\"b\"\"\",") == 100);  /* CSV quoting */
    CHECK(count_lines(path, "1000,1000001000,server,multi,0,") == 2);
    CHECK(count_lines(path, ",1000.000,8.000,OK\n") == 100);  /* 1 MB in 1 s = 8 Mbit/s */
    unlink(path);
}

int main(void) {
    test_header_and_payload_in_one_segment();
    test_fragmented_stream();
    test_large_payload_bypasses_buffer();
    test_edge_cases();
    test_proto();
    test_metrics(METRICS_SYNC);
    test_metrics(METRICS_ASYNC);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
