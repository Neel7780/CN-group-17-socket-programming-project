#define _GNU_SOURCE
#include "common.h"
#include "topics.h"
#include "proto.h"
#include "session.h"
#include "fault.h"
#include "transfer.h"
#include "metrics.h"

static topic_map_t g_topic_map;
static double g_p = 0.0;
static long g_seed = 42;
static long g_session_ttl = DEFAULT_SESSION_TTL;
static long g_max_clients = DEFAULT_MAX_CLIENTS;
static long g_sendfile = 1;
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

typedef struct {
    int client_fd;
    uint32_t conn_id;
} client_ctx_t;

/*
 * Per-connection erand48() state.
 *
 * Design Document Section 4.2, step 2 requires a per-thread random state
 * seeded with seed ^ connection id so that an experiment is reproducible.
 * The seed and the connection id are first run through a SplitMix64
 * avalanche function: without it the raw words are correlated, which makes
 * the fault decision of consecutive connections behave systematically
 * (e.g. every odd connection failing) instead of following the requested
 * Bernoulli distribution.
 */
static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void make_seed_state(unsigned short out[3], long seed, uint32_t conn_id) {
    uint64_t mixed = splitmix64((uint64_t)seed * 0x100000001B3ULL + (uint64_t)conn_id);
    out[0] = (unsigned short)(mixed & 0xFFFF);
    out[1] = (unsigned short)((mixed >> 16) & 0xFFFF);
    out[2] = (unsigned short)((mixed >> 32) & 0xFFFF);
}

/*
 * Handles a fresh transfer: allocate a session, announce it, then stream
 * the topic from the beginning.
 */
static void serve_get(int fd, const proto_request_t *req, uint32_t conn_id,
                      unsigned short seed_state[3], session_t **s_out) {
    const topic_t *top = topic_lookup(&g_topic_map, req->topic);
    if (!top) {
        proto_send_err(fd, PROTO_ERR_NOT_FOUND, "NO SUCH TOPIC");
        return;
    }

    session_t *s = session_create(top);
    if (!s) {
        proto_send_err(fd, 500, "SERVER ERROR");
        return;
    }
    *s_out = s;

    /* R = payload + header bytes of the whole topic response. */
    fault_plan_t fp;
    fault_plan_init(&fp, g_p, top->total_bytes, top->total_hdr_bytes, seed_state);

    if (proto_send_session(fd, s->sid, top->file_count, top->total_bytes) != 0) {
        log_msg(LOG_INFO, "Conn %u: failed to send SESSION", conn_id);
        session_release(s);
        *s_out = NULL;
        return;
    }

    log_msg(LOG_INFO, "Conn %u: session %s established for topic %s (%u files, %" PRIu64 " bytes)",
            conn_id, s->sid, top->key, top->file_count, top->total_bytes);

    uint64_t t_start = now_ms();
    int res = transfer_send_topic(fd, top, 0, 0, &fp, s, g_sendfile != 0);
    uint64_t dur = now_ms() - t_start;

    if (res != 0) {
        metrics_log_row("server", "TRANSFER_FAIL", s->sid, conn_id, top->key, 0, 0, dur, "FAULT_OR_ERR");
        log_msg(LOG_INFO, "Conn %u: transfer interrupted, session %s retained for resume",
                conn_id, s->sid);
        session_release(s);
        *s_out = NULL;
        return;
    }

    metrics_log_row("server", "TRANSFER_OK", s->sid, conn_id, top->key, 0, top->total_bytes, dur, "SUCCESS");
    log_msg(LOG_INFO, "Conn %u: transfer complete for session %s", conn_id, s->sid);

    /* Keep the session alive; it is released on DONE or reclaimed by the reaper. */
    session_release(s);
    *s_out = NULL;
}

/*
 * Handles a resumption: validate the client's checkpoint against the
 * session's high-water mark and continue streaming from that position.
 */
static void serve_resume(int fd, const proto_request_t *req, uint32_t conn_id,
                         unsigned short seed_state[3], session_t **s_out) {
    session_t *s = session_lookup(req->sid);
    if (!s) {
        log_msg(LOG_INFO, "Conn %u: RESUME for unknown session %s", conn_id, req->sid);
        proto_send_err(fd, PROTO_ERR_SESSION_EXPIRED, "SESSION EXPIRED");
        return;
    }
    *s_out = s;

    uint64_t remaining = 0;
    int vres = session_validate_resume(s, req->topic, req->file_idx, req->offset, &remaining);
    if (vres != 0) {
        proto_send_err(fd, vres,
                       vres == PROTO_ERR_TOPIC_MISMATCH ? "TOPIC MISMATCH" : "BAD CHECKPOINT");
        metrics_log_row("server", "RESUME_REJECT", s->sid, conn_id, s->topic->key,
                        req->file_idx, 0, 0, "REJECTED");
        session_release(s);
        *s_out = NULL;
        return;
    }

    const topic_t *top = s->topic;

    /*
     * The response size for this request covers only the bytes that still
     * have to be delivered, including the headers of the remaining files.
     */
    uint64_t rem_payload = topic_stream_size_remaining(top, req->file_idx, req->offset);
    uint64_t rem_hdr = topic_hdr_size(top, req->file_idx, req->offset);
    fault_plan_t fp;
    fault_plan_init(&fp, g_p, rem_payload, rem_hdr, seed_state);

    if (proto_send_resumed(fd, req->file_idx, req->offset, remaining) != 0) {
        session_release(s);
        *s_out = NULL;
        return;
    }

    uint64_t t_start = now_ms();
    int res = transfer_send_topic(fd, top, req->file_idx, req->offset, &fp, s, g_sendfile != 0);
    uint64_t dur = now_ms() - t_start;

    if (res != 0) {
        metrics_log_row("server", "RESUME_FAIL", s->sid, conn_id, top->key,
                        req->file_idx, 0, dur, "FAULT_OR_ERR");
        log_msg(LOG_INFO, "Conn %u: resumed transfer interrupted, session %s retained",
                conn_id, s->sid);
        session_release(s);
        *s_out = NULL;
        return;
    }

    metrics_log_row("server", "RESUME_OK", s->sid, conn_id, top->key,
                    req->file_idx, remaining, dur, "SUCCESS");
    log_msg(LOG_INFO, "Conn %u: resumed transfer complete for session %s", conn_id, s->sid);

    session_release(s);
    *s_out = NULL;
}

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    uint32_t conn_id = ctx->conn_id;
    free(ctx);

    sock_disable_sigpipe(fd);

    if (g_sendfile == 0) {
        set_tcp_cork(fd, 0);
    }

    /* 10 second read timeout so a stalled client cannot hang a worker. */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned short seed_state[3];
    make_seed_state(seed_state, g_seed, conn_id);

    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE];
    ssize_t len = lr_readline(&lr, line, sizeof(line));
    if (len <= 0) {
        close(fd);
        dec_active_clients();
        return NULL;
    }

    proto_request_t req;
    if (proto_parse_request(line, &req) != 0) {
        proto_send_err(fd, PROTO_ERR_BAD_REQUEST, "BAD REQUEST");
        close(fd);
        dec_active_clients();
        return NULL;
    }

    session_t *s = NULL;

    switch (req.type) {
        case CMD_GET:
            serve_get(fd, &req, conn_id, seed_state, &s);
            break;
        case CMD_RESUME:
            serve_resume(fd, &req, conn_id, seed_state, &s);
            break;
        case CMD_DONE:
            /* The client confirms completion; release the session state. */
            proto_send_bye(fd);
            log_msg(LOG_INFO, "Conn %u: DONE for session %s", conn_id, req.sid);
            session_destroy(req.sid);
            break;
        default:
            proto_send_err(fd, PROTO_ERR_BAD_REQUEST, "BAD REQUEST");
            break;
    }

    if (s) session_release(s);
    close(fd);
    dec_active_clients();
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability> [opts]\n", argv[0]);
        fprintf(stderr, "Options: seed=, session_ttl=, max_clients=, sendfile=, log=0..3\n");
        return EXIT_USAGE;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid port %s\n", argv[1]);
        return EXIT_USAGE;
    }

    const char *topics_root_dir = argv[2];
    g_p = atof(argv[3]);
    if (g_p < 0.0 || g_p > 1.0) {
        fprintf(stderr, "Error: failure_probability must be in range [0.0, 1.0]\n");
        return EXIT_USAGE;
    }

    for (int i = 4; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "seed", &v)) g_seed = v;
        else if (parse_kv_opt(argv[i], "session_ttl", &v)) g_session_ttl = v;
        else if (parse_kv_opt(argv[i], "max_clients", &v)) g_max_clients = v;
        else if (parse_kv_opt(argv[i], "sendfile", &v)) g_sendfile = v;
        else if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (int)v;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    metrics_init("server_metrics.csv");

    if (topic_map_build(&g_topic_map, topics_root_dir) != 0) {
        fprintf(stderr, "Error: Failed to index topics from directory %s\n", topics_root_dir);
        return EXIT_IO_ERROR;
    }

    session_table_init(g_session_ttl);

    pthread_t reaper_tid;
    if (pthread_create(&reaper_tid, NULL, session_reaper_thread, NULL) != 0) {
        perror("pthread_create(reaper)");
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

    log_msg(LOG_INFO, "Part II Case 2 Server listening on port %d, p=%.2f, sendfile=%ld, ttl=%ld",
            port, g_p, g_sendfile, g_session_ttl);

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
        if (g_active_clients >= (uint32_t)g_max_clients) {
            pthread_mutex_unlock(&g_conn_mu);
            log_msg(LOG_INFO, "Connection dropped: max clients reached (%ld)", g_max_clients);
            proto_send_err(client_fd, 503, "SERVER BUSY");
            close(client_fd);
            continue;
        }
        g_active_clients++;
        uint32_t cid = ++g_conn_counter;
        pthread_mutex_unlock(&g_conn_mu);

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) {
            close(client_fd);
            dec_active_clients();
            continue;
        }
        ctx->client_fd = client_fd;
        ctx->conn_id = cid;

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
    session_reaper_stop();
    pthread_join(reaper_tid, NULL);
    topic_map_free(&g_topic_map);
    metrics_close();
    return EXIT_OK;
}
