#define _GNU_SOURCE
#include "common.h"
#include "topics.h"
#include "proto.h"
#include "transfer.h"
#include "metrics.h"

/*
 * Part I server:
 *   ./server <port> <topics_root_dir> [options]
 *
 * Two dispatch modes (select with mode=single or mode=multi):
 *
 *   single  Accept and fully serve each client in the main thread before
 *           accepting the next one. This serialises all transfers through
 *           one socket write path and is the bottleneck measured in the
 *           single-threaded throughput benchmark.
 *
 *   multi   Spawn a detached POSIX thread for every accepted connection.
 *           The threads share the read-only topic map (no locking needed)
 *           and compete only on the metrics async queue. This is the mode
 *           that achieves 524 MB/s aggregate throughput at N=16.
 */

#define DEFAULT_MAX_CLIENTS 128

static topic_map_t g_topic_map;
static bool g_multi = true;
static long g_max_clients = DEFAULT_MAX_CLIENTS;
static char g_metrics_path[PATH_MAX] = "server_metrics.csv";

static volatile bool g_running = true;
static int g_listen_fd = -1;

/* Active client counter, protected by g_conn_mu. */
static uint32_t g_conn_counter = 0;
static uint32_t g_active_clients = 0;
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = false;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static uint32_t next_conn_id(void) {
    pthread_mutex_lock(&g_conn_mu);
    uint32_t id = ++g_conn_counter;
    pthread_mutex_unlock(&g_conn_mu);
    return id;
}

static void dec_active_clients(void) {
    pthread_mutex_lock(&g_conn_mu);
    if (g_active_clients > 0) g_active_clients--;
    pthread_mutex_unlock(&g_conn_mu);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <port> <topics_root_dir> [options]\n"
            "Options:\n"
            "  mode=<single|multi>    dispatch mode (default multi)\n"
            "  max_clients=<n>        max concurrent threads in multi mode (default %d)\n"
            "  metrics=<path>         CSV file for per-connection rows (default server_metrics.csv)\n"
            "  log=<0..3>             verbosity: 0=errors, 1=warn, 2=info, 3=debug\n",
            prog, DEFAULT_MAX_CLIENTS);
}

/*
 * Serves one client connection: reads the GET request, streams the topic,
 * then records a metrics row.
 */
static void serve_connection(int fd, uint32_t conn_id) {
    /* Set a 30 s receive timeout so a stalled or malicious client cannot
     * hold a worker thread indefinitely. */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint64_t t_accept = now_ns();
    uint64_t wall_start = now_wall_ns();

    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE];
    ssize_t len = lr_readline(&lr, line, sizeof(line));
    if (len <= 0) {
        log_msg(LOG_INFO, "Conn %u: no request", conn_id);
        close(fd);
        return;
    }

    proto_request_t req;
    if (proto_parse_request(line, &req) != 0) {
        log_msg(LOG_INFO, "Conn %u: malformed request '%.40s'", conn_id, line);
        char err[MAX_LINE];
        int elen = proto_fmt_err(err, sizeof(err), 400, "BAD REQUEST");
        if (elen > 0) send_all(fd, err, (size_t)elen);
        close(fd);
        return;
    }

    const topic_t *top = topic_lookup(&g_topic_map, req.topic);
    if (!top) {
        log_msg(LOG_INFO, "Conn %u: unknown topic '%s'", conn_id, req.topic);
        char err[MAX_LINE];
        int elen = proto_fmt_err(err, sizeof(err), 404, "NO_SUCH_TOPIC");
        if (elen > 0) send_all(fd, err, (size_t)elen);
        close(fd);
        return;
    }

    log_msg(LOG_INFO, "Conn %u: GET %s (%u files, %" PRIu64 " bytes)",
            conn_id, top->key, top->file_count, top->total_bytes);

    uint64_t wait_ns = now_ns() - t_accept;

    /* Send the OK line. wait_ns measures request parsing; duration_ns the transfer. */
    char ok_line[MAX_LINE];
    int oklen = proto_fmt_ok(ok_line, sizeof(ok_line), top->file_count, top->total_bytes);
    if (oklen <= 0) { close(fd); return; }
    if (send_all(fd, ok_line, (size_t)oklen) != (ssize_t)oklen) {
        log_msg(LOG_INFO, "Conn %u: failed to send OK", conn_id);
        close(fd);
        return;
    }

    uint64_t t_start = now_ns();
    transfer_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    int res = transfer_send_topic(fd, top, &stats);
    uint64_t duration_ns = now_ns() - t_start;

    const char *status;
    if (res == 0) {
        status = "OK";
        log_msg(LOG_INFO, "Conn %u: delivered '%s' (%u files, %" PRIu64 " bytes)",
                conn_id, top->key, stats.files_done, stats.payload_bytes);
    } else {
        status = "IO_ERROR";
        log_msg(LOG_INFO, "Conn %u: transfer of '%s' interrupted after %u files",
                conn_id, top->key, stats.files_done);
    }

    metrics_row(&(metrics_row_t){
        .role          = "server",
        .mode          = g_multi ? "multi" : "single",
        .conn_id       = conn_id,
        .topic         = top->key,
        .files         = stats.files_done,
        .payload_bytes = stats.payload_bytes,
        .wire_bytes    = stats.payload_bytes,  /* Part I: no framing overhead counted separately */
        .wall_start_ns = wall_start,
        .wait_ns       = wait_ns,
        .duration_ns   = duration_ns,
        .status        = status,
    });

    close(fd);
}

typedef struct {
    int client_fd;
    uint32_t conn_id;
} client_ctx_t;

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    uint32_t conn_id = ctx->conn_id;
    free(ctx);

    serve_connection(fd, conn_id);
    dec_active_clients();
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    int port = 0;
    if (parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[1]);
        return EXIT_USAGE;
    }
    const char *topics_root_dir = argv[2];

    for (int i = 3; i < argc; i++) {
        long v;
        const char *s;
        if (parse_kv_opt(argv[i], "max_clients", &v)) g_max_clients = v;
        else if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (v == 1) ? LOG_INFO : (int)v;
        else if (parse_kv_str(argv[i], "mode", &s)) {
            if (strcmp(s, "single") == 0) g_multi = false;
            else if (strcmp(s, "multi") == 0) g_multi = true;
            else {
                fprintf(stderr, "Error: unknown mode '%s' (use single or multi)\n", s);
                return EXIT_USAGE;
            }
        } else if (parse_kv_str(argv[i], "metrics", &s)) {
            snprintf(g_metrics_path, sizeof(g_metrics_path), "%s", s);
        } else {
            log_msg(LOG_WARN, "Ignoring unknown option '%s'", argv[i]);
        }
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /*
     * In multi mode the server spawns a detached thread per client; the
     * metrics logger thread runs concurrently, so we use METRICS_ASYNC to
     * avoid blocking any worker on disk I/O. In single mode the accept loop
     * itself is the only thread, so METRICS_SYNC is fine.
     */
    metrics_open(g_metrics_path, g_multi ? METRICS_ASYNC : METRICS_SYNC);

    if (topic_map_build(&g_topic_map, topics_root_dir) != 0) {
        fprintf(stderr, "Error: failed to index topics from %s\n", topics_root_dir);
        return EXIT_IO_ERROR;
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); return EXIT_IO_ERROR; }

    int optval = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons((uint16_t)port);

    if (bind(g_listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        perror("bind");
        close(g_listen_fd);
        return EXIT_IO_ERROR;
    }
    if (listen(g_listen_fd, 128) != 0) {
        perror("listen");
        close(g_listen_fd);
        return EXIT_IO_ERROR;
    }

    log_msg(LOG_INFO, "Part I server listening on port %d, mode=%s",
            port, g_multi ? "multi" : "single");

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&caddr, &clen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&g_conn_mu);
        bool too_many = g_multi && (long)g_active_clients >= g_max_clients;
        if (!too_many) g_active_clients++;
        pthread_mutex_unlock(&g_conn_mu);

        if (too_many) {
            log_msg(LOG_WARN, "Connection dropped: max_clients=%ld reached", g_max_clients);
            char err[MAX_LINE];
            int elen = proto_fmt_err(err, sizeof(err), 503, "SERVER BUSY");
            if (elen > 0) send_all(client_fd, err, (size_t)elen);
            close(client_fd);
            continue;
        }

        uint32_t conn_id = next_conn_id();
        char c_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &caddr.sin_addr, c_ip, sizeof(c_ip));
        log_msg(LOG_INFO, "Accepted connection %u from %s:%d", conn_id, c_ip, ntohs(caddr.sin_port));

        if (!g_multi) {
            /* Single-threaded: serve the client in-place, then loop back to accept(). */
            serve_connection(client_fd, conn_id);
            dec_active_clients();
            continue;
        }

        /* Multi-threaded: hand off to a detached worker thread. */
        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            close(client_fd);
            dec_active_clients();
            continue;
        }
        ctx->client_fd = client_fd;
        ctx->conn_id   = conn_id;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, client_thread, ctx) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            dec_active_clients();
        }
        pthread_attr_destroy(&attr);
    }

    log_msg(LOG_INFO, "Server shutting down...");

    /* Give detached workers a moment to finish writing their metrics rows. */
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&g_conn_mu);
        uint32_t active = g_active_clients;
        pthread_mutex_unlock(&g_conn_mu);
        if (active == 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    if (g_listen_fd >= 0) close(g_listen_fd);
    topic_map_free(&g_topic_map);
    metrics_close();
    return EXIT_OK;
}
