#define _GNU_SOURCE
#include "common.h"
#include "proto.h"
#include "metrics.h"

/*
 * Part II Case 1 client: no session management, restart from the beginning.
 *
 *   ./client <server_ip> <server_port> <topic> <output_dir> [options]
 *
 * The client sends GET <topic> and stores every file of the topic in
 * <output_dir>/<topic>/. If the connection dies before the closing END
 * delimiter arrives, the client treats the attempt as a failure: it deletes
 * the files it wrote in that attempt, waits (backoff starting at 100 ms and
 * doubling up to 2 s), reconnects and sends the same GET <topic> again, up to
 * max_retries attempts. An ERR reply is never retried.
 */

#define DEFAULT_METRICS_PATH "client_metrics.csv"

typedef struct {
    const char *server_ip;
    int port;
    const char *topic;
    const char *output_dir;
    const char *metrics_path;  /* "none" disables metrics */
    const char *tag;           /* Free-form label copied into the metrics "mode" column */
    long timeout_s;            /* Receive timeout; 0 waits forever */
    long max_retries;          /* Maximum number of GET attempts */
    long backoff_ms;           /* Initial backoff */
    long backoff_max_ms;       /* Backoff ceiling */
} client_opts_t;

/* The names of the files received during the current attempt. */
typedef struct {
    char **names;
    size_t count;
    size_t cap;
} name_list_t;

typedef enum {
    ATTEMPT_OK = 0,   /* the whole topic arrived */
    ATTEMPT_RETRY,    /* the connection died: reconnect and start over */
    ATTEMPT_FATAL     /* the server refused the request: do not retry */
} attempt_result_t;

/* Accumulated transfer statistics, reported in the metrics row. */
typedef struct {
    uint64_t payload_bytes;    /* File bytes received over all attempts */
    uint64_t wire_bytes;       /* All bytes read from the socket */
    uint32_t attempts;         /* Connection attempts */
    uint64_t wait_ns;          /* Start of the run until the first OK */
    uint64_t duration_ns;      /* First OK until the end of the run */
    int err_code;              /* Error code of a fatal ERR reply */
    const char *status;
} xfer_stats_t;

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <server_ip> <server_port> <topic> <output_dir> [options]\n"
            "Options:\n"
            "  log=<0..3>        stderr verbosity: 0 error, 1 warn, 2 info (default), 3 debug\n"
            "  timeout=<sec>     receive timeout in seconds (default 0 = none)\n"
            "  max_retries=<n>   maximum number of GET attempts (default %d)\n"
            "  backoff_ms=<n>    initial retry backoff (default %d)\n"
            "  backoff_max_ms=<n>  backoff ceiling (default %d)\n"
            "  metrics=<path>    CSV file for the transfer row (default %s, 'none' to disable)\n"
            "  tag=<label>       label written to the metrics 'mode' column (default '-')\n",
            prog, DEFAULT_MAX_RETRIES, DEFAULT_BACKOFF_MS, DEFAULT_BACKOFF_MAX_MS,
            DEFAULT_METRICS_PATH);
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
        log_msg(LOG_INFO, "Connection closed by the server while waiting for %s", expected);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        log_msg(LOG_INFO, "Timed out waiting for %s", expected);
    } else {
        log_msg(LOG_INFO, "Failed to read %s: %s", expected, strerror(errno));
    }
}

/* Records a file name so that a failed attempt can delete what it wrote. */
static void names_add(name_list_t *nl, const char *name) {
    if (nl->count == nl->cap) {
        size_t cap = nl->cap ? nl->cap * 2 : 32;
        char **n = realloc(nl->names, cap * sizeof(char *));
        if (!n) return;
        nl->names = n;
        nl->cap = cap;
    }
    char *copy = strdup(name);
    if (!copy) return;
    nl->names[nl->count++] = copy;
}

static void names_free(name_list_t *nl) {
    for (size_t i = 0; i < nl->count; i++) free(nl->names[i]);
    free(nl->names);
    nl->names = NULL;
    nl->count = nl->cap = 0;
}

/*
 * Deletes the files of the given names, both the finished ones and the
 * partial ".part" files. Case 1 keeps no metadata, so everything a failed
 * attempt wrote has to go: the next attempt starts the topic from byte 0.
 */
static void purge_files(const char *topic_dir, name_list_t *nl) {
    char path[PATH_MAX];
    for (size_t i = 0; i < nl->count; i++) {
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", topic_dir, nl->names[i]) < sizeof(path)) {
            unlink(path);
        }
        if ((size_t)snprintf(path, sizeof(path), "%s/%s.part", topic_dir, nl->names[i]) < sizeof(path)) {
            unlink(path);
        }
    }
}

/*
 * Removes the partial files of an earlier, interrupted run. Only ".part"
 * files are touched: a complete file of an earlier run is simply overwritten
 * by the next attempt, which renames the fresh copy over it.
 */
static void purge_stale_parts(const char *topic_dir) {
    DIR *d = opendir(topic_dir);
    if (!d) return;
    struct dirent *de;
    char path[PATH_MAX];
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen > 5 && strcmp(de->d_name + nlen - 5, ".part") == 0) {
            if ((size_t)snprintf(path, sizeof(path), "%s/%s", topic_dir, de->d_name) < sizeof(path)) {
                unlink(path);
            }
        }
    }
    closedir(d);
}

/*
 * Receives one file body into <dir>/<name>.part and renames it into place
 * once it is complete. An interrupted file is removed, so a failed attempt
 * never leaves a truncated file behind.
 */
static attempt_result_t recv_one_file(line_reader_t *lr, const char *topic_dir, const char *name,
                                      uint64_t size, char *buf, uint64_t *attempt_payload,
                                      xfer_stats_t *st) {
    char part_path[PATH_MAX];
    char final_path[PATH_MAX];
    if ((size_t)snprintf(part_path, sizeof(part_path), "%s/%s.part", topic_dir, name) >= sizeof(part_path) ||
        (size_t)snprintf(final_path, sizeof(final_path), "%s/%s", topic_dir, name) >= sizeof(final_path)) {
        log_msg(LOG_ERR, "Output path too long for file %s", name);
        st->status = "IO";
        return ATTEMPT_FATAL;
    }

    int fd = open(part_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_msg(LOG_ERR, "Cannot create %s: %s", part_path, strerror(errno));
        st->status = "IO";
        return ATTEMPT_FATAL;
    }

    uint64_t left = size;
    while (left > 0) {
        size_t want = left < IO_CHUNK ? (size_t)left : IO_CHUNK;
        ssize_t n = lr_read_exact(lr, buf, want);
        if (n != (ssize_t)want) {
            if (n < 0) log_msg(LOG_INFO, "Read failed inside %s: %s", name, strerror(errno));
            else log_msg(LOG_INFO, "Connection closed inside %s (%" PRIu64 " of %" PRIu64 " bytes)",
                         name, size - left + (uint64_t)n, size);
            close(fd);
            unlink(part_path);
            st->status = "EOF";
            return ATTEMPT_RETRY;
        }
        if (write_all(fd, buf, want) < 0) {
            log_msg(LOG_ERR, "Write to %s failed: %s", part_path, strerror(errno));
            close(fd);
            unlink(part_path);
            st->status = "IO";
            return ATTEMPT_FATAL;
        }
        left -= want;
        *attempt_payload += want;
        st->payload_bytes += want;
    }

    if (close(fd) != 0 || rename(part_path, final_path) != 0) {
        log_msg(LOG_ERR, "Cannot finalise %s: %s", final_path, strerror(errno));
        unlink(part_path);
        st->status = "IO";
        return ATTEMPT_FATAL;
    }
    log_msg(LOG_DEBUG, "Received %s (%" PRIu64 " bytes)", name, size);
    return ATTEMPT_OK;
}

/*
 * Reads the file_count FILE blocks and the closing END, checking them
 * against the totals announced by OK.
 */
static attempt_result_t recv_files(line_reader_t *lr, const char *topic_dir, uint32_t file_count,
                                   uint64_t total_bytes, char *buf, name_list_t *names,
                                   uint64_t *attempt_payload, uint32_t *attempt_files,
                                   xfer_stats_t *st) {
    char line[MAX_LINE];
    char name[MAX_NAME_LEN + 1];

    for (uint32_t i = 0; i < file_count; i++) {
        ssize_t r = lr_readline(lr, line, sizeof(line));
        if (r <= 0) {
            log_stream_break(r, "FILE header");
            st->status = "EOF";
            return ATTEMPT_RETRY;
        }

        uint64_t size;
        if (proto_parse_file_hdr(line, &size, name, sizeof(name)) != 0) {
            log_msg(LOG_ERR, "Expected FILE header #%" PRIu32 ", got: %.80s", i + 1, line);
            st->status = "PROTO";
            return ATTEMPT_FATAL;
        }
        if (!proto_safe_filename(name)) {
            log_msg(LOG_ERR, "Refusing unsafe file name from the server: %.80s", name);
            st->status = "PROTO";
            return ATTEMPT_FATAL;
        }
        if (size > total_bytes - *attempt_payload) {
            log_msg(LOG_ERR, "File %s (%" PRIu64 " bytes) exceeds the announced topic total", name, size);
            st->status = "PROTO";
            return ATTEMPT_FATAL;
        }

        attempt_result_t res = recv_one_file(lr, topic_dir, name, size, buf, attempt_payload, st);
        if (res != ATTEMPT_OK) return res;

        names_add(names, name);
        (*attempt_files)++;
    }

    ssize_t r = lr_readline(lr, line, sizeof(line));
    if (r <= 0) {
        log_stream_break(r, "END");
        st->status = "EOF";
        return ATTEMPT_RETRY;
    }

    uint32_t end_count;
    uint64_t end_total;
    if (proto_parse_end(line, &end_count, &end_total) != 0) {
        log_msg(LOG_ERR, "Expected END, got: %.80s", line);
        st->status = "PROTO";
        return ATTEMPT_FATAL;
    }
    if (end_count != file_count || end_total != total_bytes) {
        log_msg(LOG_ERR, "END %" PRIu32 " %" PRIu64 " does not match OK %" PRIu32 " %" PRIu64,
                end_count, end_total, file_count, total_bytes);
        st->status = "MISMATCH";
        return ATTEMPT_FATAL;
    }
    st->status = "OK";
    return ATTEMPT_OK;
}

/*
 * Runs one GET exchange on an open connection.
 *
 * Returns ATTEMPT_OK when the whole topic arrived, ATTEMPT_RETRY when the
 * connection died before END, and ATTEMPT_FATAL when the server refused the
 * request or sent something that is not part of the protocol.
 */
static attempt_result_t fetch_topic(int fd, const client_opts_t *o, const char *topic_dir,
                                     char *buf, name_list_t *names, xfer_stats_t *st,
                                     uint64_t *first_ok_ns, uint64_t *attempt_payload,
                                     uint32_t *attempt_files) {
    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE];
    int len = snprintf(line, sizeof(line), "GET %s\n", o->topic);
    if (send_all(fd, line, (size_t)len) != len) {
        log_msg(LOG_INFO, "Failed to send the request: %s", strerror(errno));
        st->status = "SEND";
        return ATTEMPT_RETRY;
    }
    log_msg(LOG_DEBUG, "Sent GET %s to %s:%d", o->topic, o->server_ip, o->port);

    ssize_t r = lr_readline(&lr, line, sizeof(line));
    if (r <= 0) {
        log_stream_break(r, "the OK response");
        st->status = "EOF";
        return ATTEMPT_RETRY;
    }

    int err_code = 0;
    const char *reason = NULL;
    if (proto_parse_err(line, &err_code, &reason) == 0) {
        log_msg(LOG_INFO, "Server refused topic '%s': ERR %d %s", o->topic, err_code, reason);
        st->err_code = err_code;
        st->status = (err_code == PROTO_ERR_NOT_FOUND) ? "ERR_404" :
                     (err_code == PROTO_ERR_BAD_REQUEST) ? "ERR_400" : "ERR";
        /* An ERR reply is terminal: it is never retried. */
        return ATTEMPT_FATAL;
    }

    uint32_t file_count = 0;
    uint64_t total_bytes = 0;
    if (proto_parse_ok(line, &file_count, &total_bytes) != 0) {
        log_msg(LOG_ERR, "Unexpected response: %.80s", line);
        st->status = "PROTO";
        return ATTEMPT_FATAL;
    }
    if (*first_ok_ns == 0) *first_ok_ns = now_ns();
    log_msg(LOG_INFO, "Topic '%s': %" PRIu32 " files, %" PRIu64 " bytes", o->topic, file_count, total_bytes);

    if (ensure_dir_exists(topic_dir) != 0) {
        log_msg(LOG_ERR, "Cannot create %s: %s", topic_dir, strerror(errno));
        st->status = "IO";
        return ATTEMPT_FATAL;
    }

    attempt_result_t res = recv_files(&lr, topic_dir, file_count, total_bytes, buf, names,
                                      attempt_payload, attempt_files, st);
    st->wire_bytes += lr.bytes_in;
    return res;
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
        .max_retries = DEFAULT_MAX_RETRIES,
        .backoff_ms = DEFAULT_BACKOFF_MS,
        .backoff_max_ms = DEFAULT_BACKOFF_MAX_MS,
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
    if (o.max_retries < 1) {
        fprintf(stderr, "Error: max_retries must be at least 1\n");
        return EXIT_USAGE;
    }

    for (int i = 5; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (v == 1) ? LOG_INFO : (int)v;
        else if (parse_kv_opt(argv[i], "timeout", &v) && v >= 0) o.timeout_s = v;
        else if (parse_kv_opt(argv[i], "max_retries", &v) && v >= 1) o.max_retries = v;
        else if (parse_kv_opt(argv[i], "backoff_ms", &v) && v >= 0) o.backoff_ms = v;
        else if (parse_kv_opt(argv[i], "backoff_max_ms", &v) && v > 0) o.backoff_max_ms = v;
        else if (parse_kv_str(argv[i], "metrics", &o.metrics_path)) continue;
        else if (parse_kv_str(argv[i], "tag", &o.tag)) continue;
        else log_msg(LOG_WARN, "Ignoring unknown option '%s'", argv[i]);
    }

    signal(SIGPIPE, SIG_IGN);

    if (strcmp(o.metrics_path, "none") != 0) {
        metrics_open(o.metrics_path, METRICS_SYNC);
    }

    char topic_dir[PATH_MAX];
    if ((size_t)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", o.output_dir, o.topic) >= sizeof(topic_dir)) {
        fprintf(stderr, "Error: output directory path too long\n");
        return EXIT_USAGE;
    }

    char *buf = malloc(IO_CHUNK);
    if (!buf) {
        fprintf(stderr, "Error: out of memory\n");
        return EXIT_IO_ERROR;
    }

    xfer_stats_t st;
    memset(&st, 0, sizeof(st));
    st.status = "ERR";

    uint64_t t_run_start = now_ns();
    uint64_t first_ok_ns = 0;
    uint64_t useful_bytes = 0;
    uint32_t files_done = 0;

    long backoff = o.backoff_ms;
    int rc = EXIT_RETRY_LIMIT;

    for (long attempt = 1; attempt <= o.max_retries; attempt++) {
        st.attempts++;

        /* Remove the partial files of an earlier, interrupted run. */
        purge_stale_parts(topic_dir);

        name_list_t names = { NULL, 0, 0 };
        uint64_t attempt_payload = 0;
        uint32_t attempt_files = 0;

        int fd = connect_to_server(o.server_ip, o.port);
        if (fd < 0) {
            st.status = "CONNECT";
            log_msg(LOG_INFO, "Attempt %ld/%ld: could not connect, retrying in %ld ms",
                    attempt, o.max_retries, backoff);
            goto backoff;
        }

        if (o.timeout_s > 0) {
            struct timeval tv = { .tv_sec = o.timeout_s, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        attempt_result_t res = fetch_topic(fd, &o, topic_dir, buf, &names, &st, &first_ok_ns,
                                           &attempt_payload, &attempt_files);
        close(fd);

        if (res == ATTEMPT_OK) {
            /* The topic arrived completely: this attempt's payload is useful. */
            useful_bytes = attempt_payload;
            files_done = attempt_files;
            rc = EXIT_OK;
            names_free(&names);
            break;
        }

        if (res == ATTEMPT_FATAL) {
            if (st.err_code == PROTO_ERR_NOT_FOUND || st.err_code == PROTO_ERR_BAD_REQUEST) {
                fprintf(stderr, "Error: topic '%s' was refused by the server\n", o.topic);
                rc = EXIT_TOPIC_NOT_FOUND;
            } else {
                fprintf(stderr, "Error: transfer of '%s' failed: %s\n", o.topic,
                        st.status ? st.status : "ERR");
                rc = EXIT_IO_ERROR;
            }
            names_free(&names);
            break;
        }

        /*
         * The connection died before END. Everything this attempt wrote is
         * useless, because the next attempt restarts the topic from byte 0.
         */
        purge_files(topic_dir, &names);
        names_free(&names);

        log_msg(LOG_INFO, "Attempt %ld/%ld: the transfer was interrupted, retrying in %ld ms",
                attempt, o.max_retries, backoff);

    backoff:
        if (attempt < o.max_retries) {
            struct timespec ts = {
                .tv_sec = backoff / 1000,
                .tv_nsec = (backoff % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);
            backoff = (backoff * 2 < o.backoff_max_ms) ? backoff * 2 : o.backoff_max_ms;
        }
    }

    uint64_t t_end = now_ns();
    st.wait_ns = (first_ok_ns > 0) ? (first_ok_ns - t_run_start) : (t_end - t_run_start);
    st.duration_ns = (first_ok_ns > 0) ? (t_end - first_ok_ns) : (t_end - t_run_start);

    if (rc == EXIT_RETRY_LIMIT) {
        fprintf(stderr, "Error: transfer of '%s' did not complete after %u attempts\n",
                o.topic, st.attempts);
        st.status = "RETRY_LIMIT";
    }

    /*
     * The payload received by the failed attempts was thrown away, so it is
     * exactly the redundant traffic of a run without session management.
     */
    uint64_t redundant = (st.payload_bytes > useful_bytes) ? (st.payload_bytes - useful_bytes) : 0;

    metrics_row(&(metrics_row_t){
        .role = "client",
        .mode = o.tag,
        .conn_id = (uint64_t)getpid(),
        .topic = o.topic,
        .files = files_done,
        .payload_bytes = useful_bytes,
        .wire_bytes = st.wire_bytes,
        .useful_bytes = useful_bytes,
        .redundant_bytes = redundant,
        .attempts = st.attempts,
        .wall_start_ns = now_wall_ns() - (st.wait_ns + st.duration_ns),
        .wait_ns = st.wait_ns,
        .duration_ns = st.duration_ns,
        .status = st.status ? st.status : "ERR",
    });
    metrics_close();

    if (rc == EXIT_OK) {
        double secs = (double)st.duration_ns / 1e9;
        printf("%s: %" PRIu32 " files, %" PRIu64 " bytes in %.3f ms (%.2f Mbit/s), "
               "%u attempts, %" PRIu64 " redundant bytes\n",
               o.topic, files_done, useful_bytes, secs * 1e3,
               secs > 0 ? (double)useful_bytes * 8.0 / secs / 1e6 : 0.0,
               st.attempts, redundant);
    }

    free(buf);
    return rc;
}
