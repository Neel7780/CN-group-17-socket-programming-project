#define _GNU_SOURCE
#include "common.h"
#include "proto.h"
#include "metrics.h"

/*
 * Part I client:
 *   ./client <server_ip> <server_port> <topic> <output_dir> [log=N] [timeout=S] [metrics=PATH] [tag=T]
 *
 * Sends GET <topic>, receives every file of the topic into <output_dir>/<topic>/,
 * and exits 0 only when END arrives and matches the counts announced by OK.
 */

#define DEFAULT_METRICS_PATH "client_metrics.csv"

typedef struct {
    const char *server_ip;
    int port;
    const char *topic;
    const char *output_dir;
    const char *metrics_path;  /* "none" disables metrics */
    const char *tag;           /* Free-form label copied into the metrics "mode" column */
    long timeout_s;            /* Receive timeout; 0 waits forever (a single-threaded server may queue us) */
} client_opts_t;

typedef struct {
    uint32_t files;            /* Files fully written to disk */
    uint64_t payload_bytes;    /* File bytes received */
    uint64_t wire_bytes;       /* All bytes read from the socket */
    uint64_t wall_start_ns;    /* Wall clock when OK arrived */
    uint64_t wait_ns;          /* connect() until OK: handshake plus server-side queueing */
    uint64_t duration_ns;      /* OK until END: the transfer itself */
    const char *status;
} xfer_stats_t;

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <server_ip> <server_port> <topic> <output_dir> [options]\n"
            "Options:\n"
            "  log=0..3        stderr verbosity: 0 error, 1 warn, 2 info (default), 3 debug\n"
            "  timeout=SEC     receive timeout in seconds (default 0 = none)\n"
            "  metrics=PATH    CSV file for per-transfer metrics (default %s, 'none' to disable)\n"
            "  tag=LABEL       label written to the metrics 'mode' column (default '-')\n",
            prog, DEFAULT_METRICS_PATH);
}

static int connect_to_server(const char *host, int port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        log_msg(LOG_ERR, "Cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        log_msg(LOG_ERR, "Cannot connect to %s:%d: %s", host, port, strerror(saved_errno));
    }
    return fd;
}

/* Logs why lr_readline() did not return a line. */
static void log_stream_break(ssize_t r, const char *expected) {
    if (r == 0) {
        log_msg(LOG_ERR, "Connection closed by server while waiting for %s", expected);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        log_msg(LOG_ERR, "Timed out waiting for %s", expected);
    } else {
        log_msg(LOG_ERR, "Failed to read %s: %s", expected, strerror(errno));
    }
}

/* Receives one file body into <dir>/<name>.part and renames it into place once complete. */
static int recv_one_file(line_reader_t *lr, const char *topic_dir, const char *name,
                         uint64_t size, char *buf, xfer_stats_t *st) {
    char part_path[PATH_MAX];
    char final_path[PATH_MAX];
    if ((size_t)snprintf(part_path, sizeof(part_path), "%s/%s.part", topic_dir, name) >= sizeof(part_path) ||
        (size_t)snprintf(final_path, sizeof(final_path), "%s/%s", topic_dir, name) >= sizeof(final_path)) {
        log_msg(LOG_ERR, "Output path too long for file %s", name);
        st->status = "IO";
        return EXIT_IO_ERROR;
    }

    int fd = open(part_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_msg(LOG_ERR, "Cannot create %s: %s", part_path, strerror(errno));
        st->status = "IO";
        return EXIT_IO_ERROR;
    }

    uint64_t left = size;
    while (left > 0) {
        size_t want = left < IO_CHUNK ? (size_t)left : IO_CHUNK;
        ssize_t n = lr_read_exact(lr, buf, want);
        if (n != (ssize_t)want) {
            if (n < 0) log_msg(LOG_ERR, "Read failed inside %s: %s", name, strerror(errno));
            else log_msg(LOG_ERR, "Connection closed inside %s (%" PRIu64 " of %" PRIu64 " bytes)",
                         name, size - left + (uint64_t)n, size);
            close(fd);
            unlink(part_path);
            st->status = "EOF";
            return EXIT_RETRY_LIMIT;
        }
        if (write_all(fd, buf, want) < 0) {
            log_msg(LOG_ERR, "Write to %s failed: %s", part_path, strerror(errno));
            close(fd);
            unlink(part_path);
            st->status = "IO";
            return EXIT_IO_ERROR;
        }
        left -= want;
        st->payload_bytes += want;
    }

    if (close(fd) != 0 || rename(part_path, final_path) != 0) {
        log_msg(LOG_ERR, "Cannot finalize %s: %s", final_path, strerror(errno));
        unlink(part_path);
        st->status = "IO";
        return EXIT_IO_ERROR;
    }
    log_msg(LOG_DEBUG, "Received %s (%" PRIu64 " bytes)", name, size);
    return EXIT_OK;
}

/* Reads file_count FILE blocks and the closing END, checking them against the OK totals. */
static int recv_files(line_reader_t *lr, const char *topic_dir, uint32_t file_count,
                      uint64_t total_bytes, xfer_stats_t *st) {
    char line[MAX_LINE];
    char name[MAX_NAME_LEN + 1];
    char *buf = malloc(IO_CHUNK);
    if (!buf) {
        st->status = "IO";
        return EXIT_IO_ERROR;
    }

    int rc = EXIT_OK;
    for (uint32_t i = 0; i < file_count && rc == EXIT_OK; i++) {
        ssize_t r = lr_readline(lr, line, sizeof(line));
        if (r <= 0) {
            log_stream_break(r, "FILE header");
            st->status = "EOF";
            rc = EXIT_RETRY_LIMIT;
            break;
        }

        uint64_t size;
        if (proto_parse_file_hdr(line, &size, name, sizeof(name)) != 0) {
            log_msg(LOG_ERR, "Expected FILE header #%" PRIu32 ", got: %.80s", i + 1, line);
            st->status = "PROTO";
            rc = EXIT_RETRY_LIMIT;
            break;
        }
        if (!proto_safe_filename(name)) {
            log_msg(LOG_ERR, "Refusing unsafe file name from server: %.80s", name);
            st->status = "PROTO";
            rc = EXIT_RETRY_LIMIT;
            break;
        }
        if (size > total_bytes - st->payload_bytes) {
            log_msg(LOG_ERR, "File %s (%" PRIu64 " bytes) exceeds announced topic total", name, size);
            st->status = "PROTO";
            rc = EXIT_RETRY_LIMIT;
            break;
        }

        rc = recv_one_file(lr, topic_dir, name, size, buf, st);
        if (rc == EXIT_OK) st->files++;
    }
    free(buf);
    if (rc != EXIT_OK) return rc;

    ssize_t r = lr_readline(lr, line, sizeof(line));
    if (r <= 0) {
        log_stream_break(r, "END");
        st->status = "EOF";
        return EXIT_RETRY_LIMIT;
    }

    uint32_t end_count;
    uint64_t end_total;
    if (proto_parse_end(line, &end_count, &end_total) != 0) {
        log_msg(LOG_ERR, "Expected END, got: %.80s", line);
        st->status = "PROTO";
        return EXIT_RETRY_LIMIT;
    }
    if (end_count != file_count || end_total != total_bytes || st->payload_bytes != total_bytes) {
        log_msg(LOG_ERR, "END %" PRIu32 " %" PRIu64 " does not match OK %" PRIu32 " %" PRIu64
                " (received %" PRIu32 " files, %" PRIu64 " bytes)",
                end_count, end_total, file_count, total_bytes, st->files, st->payload_bytes);
        st->status = "MISMATCH";
        return EXIT_RETRY_LIMIT;
    }
    st->status = "OK";
    return EXIT_OK;
}

/* Runs one GET exchange. Returns a process exit code and fills st for the metrics row. */
static int fetch_topic(const client_opts_t *o, xfer_stats_t *st) {
    char topic_dir[PATH_MAX];
    if ((size_t)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", o->output_dir, o->topic) >= sizeof(topic_dir)) {
        log_msg(LOG_ERR, "Output directory path too long");
        st->status = "IO";
        return EXIT_IO_ERROR;
    }

    uint64_t t_connect = now_ns();
    st->wall_start_ns = now_wall_ns();
    int fd = connect_to_server(o->server_ip, o->port);
    if (fd < 0) {
        st->status = "CONNECT";
        return EXIT_RETRY_LIMIT;
    }

    if (o->timeout_s > 0) {
        struct timeval tv = { .tv_sec = o->timeout_s, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    line_reader_t *lr = malloc(sizeof(*lr));
    if (!lr) {
        close(fd);
        st->status = "IO";
        return EXIT_IO_ERROR;
    }
    lr_init(lr, fd);

    int rc;
    char line[MAX_LINE];
    int len = snprintf(line, sizeof(line), "GET %s\n", o->topic);
    if (send_all(fd, line, (size_t)len) != len) {
        log_msg(LOG_ERR, "Failed to send request: %s", strerror(errno));
        st->status = "SEND";
        rc = EXIT_RETRY_LIMIT;
        goto out;
    }
    log_msg(LOG_DEBUG, "Sent GET %s to %s:%d", o->topic, o->server_ip, o->port);

    ssize_t r = lr_readline(lr, line, sizeof(line));
    uint64_t t_ok = now_ns();
    st->wall_start_ns = now_wall_ns();
    st->wait_ns = t_ok - t_connect;
    if (r <= 0) {
        log_stream_break(r, "OK response");
        st->status = "EOF";
        rc = EXIT_RETRY_LIMIT;
        goto out;
    }

    int err_code;
    const char *reason;
    if (proto_parse_err(line, &err_code, &reason) == 0) {
        log_msg(LOG_ERR, "Server refused topic '%s': ERR %d %s", o->topic, err_code, reason);
        st->status = err_code == 404 ? "ERR_404" : err_code == 400 ? "ERR_400" : "ERR";
        rc = (err_code == 404 || err_code == 400) ? EXIT_TOPIC_NOT_FOUND : EXIT_RETRY_LIMIT;
        goto out;
    }

    uint32_t file_count;
    uint64_t total_bytes;
    if (proto_parse_ok(line, &file_count, &total_bytes) != 0) {
        log_msg(LOG_ERR, "Unexpected response: %.80s", line);
        st->status = "PROTO";
        rc = EXIT_RETRY_LIMIT;
        goto out;
    }
    log_msg(LOG_INFO, "Topic '%s': %" PRIu32 " files, %" PRIu64 " bytes", o->topic, file_count, total_bytes);

    if (ensure_dir_exists(topic_dir) != 0) {
        log_msg(LOG_ERR, "Cannot create %s: %s", topic_dir, strerror(errno));
        st->status = "IO";
        rc = EXIT_IO_ERROR;
        goto out;
    }

    rc = recv_files(lr, topic_dir, file_count, total_bytes, st);
    st->duration_ns = now_ns() - t_ok;

out:
    st->wire_bytes = lr->bytes_in;
    free(lr);
    close(fd);
    return rc;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    client_opts_t o = {
        .server_ip = argv[1],
        .topic = argv[3],
        .output_dir = argv[4],
        .metrics_path = DEFAULT_METRICS_PATH,
        .tag = "-",
        .timeout_s = 0,
    };

    if (parse_port(argv[2], &o.port) != 0) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[2]);
        return EXIT_USAGE;
    }
    if (!proto_valid_topic(o.topic)) {
        fprintf(stderr, "Error: invalid topic '%s' (1-%d chars, no '/', '..' or spaces)\n",
                o.topic, MAX_TOPIC_LEN);
        return EXIT_USAGE;
    }
    if (o.output_dir[0] == '\0') {
        fprintf(stderr, "Error: output_dir must not be empty\n");
        return EXIT_USAGE;
    }

    for (int i = 5; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (int)v;
        else if (parse_kv_opt(argv[i], "timeout", &v) && v >= 0) o.timeout_s = v;
        else if (parse_kv_str(argv[i], "metrics", &o.metrics_path)) continue;
        else if (parse_kv_str(argv[i], "tag", &o.tag)) continue;
        else log_msg(LOG_WARN, "Ignoring unknown option '%s'", argv[i]);
    }

    signal(SIGPIPE, SIG_IGN);

    if (strcmp(o.metrics_path, "none") != 0) {
        metrics_open(o.metrics_path, METRICS_SYNC);
    }

    xfer_stats_t st;
    memset(&st, 0, sizeof(st));
    int rc = fetch_topic(&o, &st);

    metrics_row(&(metrics_row_t){
        .role = "client",
        .mode = o.tag,
        .conn_id = (uint64_t)getpid(),
        .topic = o.topic,
        .files = st.files,
        .payload_bytes = st.payload_bytes,
        .wire_bytes = st.wire_bytes,
        .wall_start_ns = st.wall_start_ns,
        .wait_ns = st.wait_ns,
        .duration_ns = st.duration_ns,
        .status = st.status ? st.status : "ERR",
    });
    metrics_close();

    if (rc == EXIT_OK) {
        double secs = (double)st.duration_ns / 1e9;
        printf("%s: %" PRIu32 " files, %" PRIu64 " bytes in %.3f ms (%.2f Mbit/s), waited %.3f ms\n",
               o.topic, st.files, st.payload_bytes, secs * 1e3,
               secs > 0 ? (double)st.payload_bytes * 8.0 / secs / 1e6 : 0.0,
               (double)st.wait_ns / 1e6);
    }
    return rc;
}
