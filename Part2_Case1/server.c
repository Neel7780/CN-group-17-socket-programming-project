#define _GNU_SOURCE
#include "common.h"
#include "topics.h"
#include "proto.h"
#include "fault.h"
#include "transfer.h"
#include "metrics.h"

/*
 * Part II Case 1 server: the Part I file server plus fault injection.
 *
 *   ./server <port> <topics_root_dir> <failure_probability> [options]
 *
 * For every request the server performs exactly one Bernoulli trial with
 * probability p (Design Document Section 4.1). If the trial fires, a cut
 * point K is drawn uniformly over the R response bytes and the connection is
 * aborted with a TCP RST once K bytes have been written.
 *
 * The server keeps no session state: a client that loses the connection
 * simply reconnects and starts the topic from the beginning.
 */

static topic_map_t g_topic_map;
static double g_p = DEFAULT_FAULT_PROB;
static long g_seed = DEFAULT_SEED;
static fault_mode_t g_fault_mode = FAULT_MODE_REQUEST;
static long g_max_clients = DEFAULT_MAX_CLIENTS;
static long g_sendfile = 1;
static bool g_multi = true;
static char g_metrics_path[PATH_MAX] = "server_metrics.csv";

static volatile bool g_running = true;
static int g_listen_fd = -1;

static uint32_t g_conn_counter = 0;
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_active_clients = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = false;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void dec_active_clients(void) {
    pthread_mutex_lock(&g_conn_mu);
    if (g_active_clients > 0) g_active_clients--;
    pthread_mutex_unlock(&g_conn_mu);
}

static uint32_t next_conn_id(void) {
    pthread_mutex_lock(&g_conn_mu);
    uint32_t id = ++g_conn_counter;
    pthread_mutex_unlock(&g_conn_mu);
    return id;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s <port> <topics_root_dir> <failure_probability> [options]\n"
            "Options:\n"
            "  seed=<n>            seed of the fault random stream (default %d)\n"
            "  fault_mode=<m>      request (default) or chunk\n"
            "  mode=<m>            multi (default) or single\n"
            "  max_clients=<n>     maximum concurrent client threads (default %d)\n"
            "  sendfile=<0|1>      use zero-copy sendfile() (default 1)\n"
            "  metrics=<path>      CSV file for the per-connection rows (default server_metrics.csv)\n"
            "  log=<0..3>          verbosity: 0=errors, 1=warn, 2=info, 3=debug\n",
            prog, DEFAULT_SEED, DEFAULT_MAX_CLIENTS);
}

/*
 * Serves one client connection: parses the request, plans the fault and
 * streams the topic. The whole exchange happens on the connection's own
 * thread, so a fault cut never blocks the other clients.
 */
static void serve_connection(int fd, uint32_t conn_id) {
    sock_disable_sigpipe(fd);

    /* A stalled client must not keep a worker thread forever. */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint64_t t_accept = now_ns();

    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE];
    ssize_t len = lr_readline(&lr, line, sizeof(line));
    if (len <= 0) {
        log_msg(LOG_INFO, "Conn %u: no request received", conn_id);
        close(fd);
        return;
    }

    proto_request_t req;
    if (proto_parse_request(line, &req) != 0) {
        log_msg(LOG_INFO, "Conn %u: malformed request '%.40s'", conn_id, line);
        char err[MAX_LINE];
        int elen = proto_fmt_err(err, sizeof(err), PROTO_ERR_BAD_REQUEST, "BAD REQUEST");
        if (elen > 0) send_all(fd, err, (size_t)elen);
        close(fd);
        return;
    }

    const topic_t *top = topic_lookup(&g_topic_map, req.topic);
    if (!top) {
        log_msg(LOG_INFO, "Conn %u: unknown topic '%s'", conn_id, req.topic);
        char err[MAX_LINE];
        int elen = proto_fmt_err(err, sizeof(err), PROTO_ERR_NOT_FOUND, "NO_SUCH_TOPIC");
        if (elen > 0) send_all(fd, err, (size_t)elen);
        close(fd);
        return;
    }

    /*
     * R, the exact size of the response this request is about to produce:
     * the OK line, every FILE header, all payload bytes and the END line.
     * The cut point is drawn uniformly over those bytes.
     */
    uint64_t response_bytes = topic_response_size(top);

    unsigned short seed_state[3];
    fault_seed_state(seed_state, g_seed, conn_id);

    fault_plan_t fp;
    fault_plan_init(&fp, g_fault_mode, g_p, response_bytes, seed_state, conn_id, top->key);

    char ok[MAX_LINE];
    int oklen = proto_fmt_ok(ok, sizeof(ok), top->file_count, top->total_bytes);
    if (oklen <= 0) {
        close(fd);
        return;
    }

    ssize_t oksent = fault_send(&fp, fd, ok, (size_t)oklen);
    if (oksent != oklen) {
        log_msg(LOG_INFO, "Conn %u: connection cut before the file stream started", conn_id);
        /* The fault plan may have closed the socket already. */
        if (!fp.aborted) close(fd);
        return;
    }

    uint64_t t_start = now_ns();
    transfer_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    int res = transfer_send_topic(fd, top, &fp, g_sendfile != 0, &stats);
    uint64_t duration = now_ns() - t_start;

    const char *status;
    if (res == 0) {
        status = "OK";
        log_msg(LOG_INFO, "Conn %u: topic '%s' delivered (%u files, %" PRIu64 " bytes)",
                conn_id, top->key, stats.files_done, stats.payload_bytes);
    } else if (fp.aborted) {
        status = "FAULT";
        log_msg(LOG_INFO, "Conn %u: transfer of '%s' interrupted by an injected fault "
                "(%" PRIu64 " of %" PRIu64 " bytes sent)",
                conn_id, top->key, fp.bytes_written, response_bytes);
    } else {
        status = "IO_ERROR";
        log_msg(LOG_INFO, "Conn %u: transfer of '%s' failed: %s", conn_id, top->key, strerror(errno));
    }

    metrics_row(&(metrics_row_t){
        .role = "server",
        .mode = g_multi ? "multi" : "single",
        .conn_id = conn_id,
        .topic = top->key,
        .files = stats.files_done,
        .payload_bytes = stats.payload_bytes,
        .wire_bytes = fp.bytes_written,
        .useful_bytes = stats.payload_bytes,
        .redundant_bytes = 0,
        .attempts = 1,
        .wall_start_ns = now_wall_ns() - duration,
        .wait_ns = t_start - t_accept,
        .duration_ns = duration,
        .status = status,
    });

    if (!fp.aborted) close(fd);
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
    if (argc < 4) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    int port = 0;
    if (parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Error: invalid port '%s'\n", argv[1]);
        return EXIT_USAGE;
    }

    const char *topics_root_dir = argv[2];
    g_p = atof(argv[3]);
    if (!(g_p >= 0.0 && g_p <= 1.0)) {
        fprintf(stderr, "Error: failure_probability must be in the range [0.0, 1.0]\n");
        return EXIT_USAGE;
    }

    for (int i = 4; i < argc; i++) {
        long v;
        const char *s;
        if (parse_kv_opt(argv[i], "seed", &v)) g_seed = v;
        else if (parse_kv_opt(argv[i], "max_clients", &v)) g_max_clients = v;
        else if (parse_kv_opt(argv[i], "sendfile", &v)) g_sendfile = v;
        else if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (v == 1) ? LOG_INFO : (int)v;
        else if (parse_kv_str(argv[i], "fault_mode", &s)) {
            if (fault_mode_parse(s, &g_fault_mode) != 0) {
                fprintf(stderr, "Error: unknown fault_mode '%s' (use request or chunk)\n", s);
                return EXIT_USAGE;
            }
        } else if (parse_kv_str(argv[i], "mode", &s)) {
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
     * The dedicated logger thread is only used in multi-threaded mode: in
     * single-threaded mode the serving thread writes the row itself, so
     * logging never delays the network path (Design Document Section 1).
     */
    metrics_open(g_metrics_path, g_multi ? METRICS_ASYNC : METRICS_SYNC);

    if (topic_map_build(&g_topic_map, topics_root_dir) != 0) {
        fprintf(stderr, "Error: failed to index topics from directory %s\n", topics_root_dir);
        return EXIT_IO_ERROR;
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return EXIT_IO_ERROR;
    }

    int optval = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons((uint16_t)port);

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

    log_msg(LOG_INFO, "Part II Case 1 server listening on port %d, p=%.2f, fault_mode=%s, mode=%s, seed=%ld",
            port, g_p, fault_mode_name(g_fault_mode), g_multi ? "multi" : "single", g_seed);

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
            log_msg(LOG_INFO, "Connection dropped: max clients reached (%ld)", g_max_clients);
            char err[MAX_LINE];
            int elen = proto_fmt_err(err, sizeof(err), 503, "SERVER BUSY");
            if (elen > 0) send_all(client_fd, err, (size_t)elen);
            close(client_fd);
            continue;
        }

        uint32_t conn_id = next_conn_id();

        if (!g_multi) {
            /* Single-threaded mode: serve the client in the accept loop. */
            serve_connection(client_fd, conn_id);
            dec_active_clients();
            continue;
        }

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            close(client_fd);
            dec_active_clients();
            continue;
        }
        ctx->client_fd = client_fd;
        ctx->conn_id = conn_id;

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

    /* Give the workers a moment to finish so their metrics rows are written. */
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
