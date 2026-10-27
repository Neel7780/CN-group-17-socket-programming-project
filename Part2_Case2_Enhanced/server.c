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
static long g_sndbuf = 0;
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

typedef struct {
    int client_fd;
    uint32_t conn_id;
} client_ctx_t;

static void serve_fetch_loop(int fd, session_t *s, fault_plan_t *fp, line_reader_t *lr, uint32_t conn_id) {
    char line[MAX_LINE];
    while (g_running) {
        ssize_t len = lr_readline(lr, line, sizeof(line));
        if (len <= 0) {
            break;
        }

        proto_request_t req;
        if (proto_parse_request(line, &req) != 0) {
            proto_send_err(fd, 400, "BAD REQUEST");
            break;
        }

        if (req.type == CMD_FETCH) {
            if (strcmp(req.sid, s->sid) != 0) {
                proto_send_err(fd, 409, "SESSION MISMATCH");
                break;
            }

            log_msg(LOG_DEBUG, "Conn %u FETCH sid=%s files=[%u..%u] offset=%lu",
                    conn_id, req.sid, req.first_idx, req.last_idx, req.offset);

            uint64_t t_start = now_ms();
            int res = transfer_send_batch(fd, s->topic, req.first_idx, req.last_idx,
                                          req.offset, fp, s, g_sendfile != 0, true);
            uint64_t dur = now_ms() - t_start;

            if (res != 0) {
                metrics_log_row("server", "FETCH_FAIL", s->sid, conn_id, s->topic->key,
                                req.first_idx, 0, dur, "FAULT_OR_ERR");
                break;
            }

            metrics_log_row("server", "FETCH_OK", s->sid, conn_id, s->topic->key,
                            req.first_idx, 0, dur, "SUCCESS");
            session_update_activity(s);
        } else if (req.type == CMD_DONE) {
            log_msg(LOG_INFO, "Conn %u received DONE for sid=%s", conn_id, req.sid);
            proto_send_bye(fd);
            metrics_log_row("server", "SESSION_DONE", s->sid, conn_id, s->topic->key,
                            0, s->bytes_sent, 0, "DONE");
            session_destroy(req.sid);
            break;
        } else {
            proto_send_err(fd, 400, "UNEXPECTED COMMAND");
            break;
        }
    }
}

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    uint32_t conn_id = ctx->conn_id;
    free(ctx);

    if (g_sndbuf > 0) {
        set_socket_buffers(fd, (int)g_sndbuf, 0);
    }

    /* Set socket read timeout to 10 seconds */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned short seed_state[3] = {
        (unsigned short)(g_seed & 0xFFFF),
        (unsigned short)((g_seed >> 16) ^ conn_id),
        (unsigned short)(conn_id & 0xFFFF)
    };

    line_reader_t lr;
    lr_init(&lr, fd);

    char line[MAX_LINE];
    ssize_t len = lr_readline(&lr, line, sizeof(line));
    if (len <= 0) {
        close(fd);
        pthread_mutex_lock(&g_conn_mu);
        g_active_clients--;
        pthread_mutex_unlock(&g_conn_mu);
        return NULL;
    }

    proto_request_t req;
    if (proto_parse_request(line, &req) != 0) {
        proto_send_err(fd, 400, "BAD REQUEST");
        close(fd);
        pthread_mutex_lock(&g_conn_mu);
        g_active_clients--;
        pthread_mutex_unlock(&g_conn_mu);
        return NULL;
    }

    session_t *s = NULL;
    fault_plan_t fp;
    memset(&fp, 0, sizeof(fp));

    if (req.type == CMD_GET) {
        const topic_t *top = topic_lookup(&g_topic_map, req.topic);
        if (!top) {
            proto_send_err(fd, 404, "NO SUCH TOPIC");
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        s = session_create(top);
        if (!s) {
            proto_send_err(fd, 500, "SERVER ERROR");
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        fault_plan_init(&fp, g_p, top->total_bytes, req.n_conns, seed_state);

        /* Send SESSION response */
        char s_hdr[256];
        int s_len = snprintf(s_hdr, sizeof(s_hdr), "SESSION %s %u %llu\n",
                             s->sid, top->file_count, (unsigned long long)top->total_bytes);
        if (fault_send(fd, &fp, s_hdr, s_len) != s_len) {
            session_release(s);
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        /* Stream Manifest */
        for (uint32_t i = 0; i < top->file_count; i++) {
            char m_line[512];
            int m_len = snprintf(m_line, sizeof(m_line), "M %u %llu %s\n",
                                 i, (unsigned long long)top->files[i].size, top->files[i].name);
            if (fault_send(fd, &fp, m_line, m_len) != m_len) {
                session_release(s);
                close(fd);
                pthread_mutex_lock(&g_conn_mu);
                g_active_clients--;
                pthread_mutex_unlock(&g_conn_mu);
                return NULL;
            }
        }

        const char *mend = "MEND\n";
        if (fault_send(fd, &fp, mend, 5) != 5) {
            session_release(s);
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        log_msg(LOG_INFO, "Conn %u sent manifest for topic %s to client", conn_id, top->key);
        /* Connection 1 now serves batches via FETCH */
        serve_fetch_loop(fd, s, &fp, &lr, conn_id);

    } else if (req.type == CMD_ATTACH) {
        s = session_lookup(req.sid);
        if (!s) {
            proto_send_err(fd, 410, "SESSION EXPIRED");
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        uint64_t rem = 0;
        pthread_mutex_lock(&s->mu);
        if (s->topic->total_bytes > s->hw_pos) {
            rem = s->topic->total_bytes - s->hw_pos;
        }
        pthread_mutex_unlock(&s->mu);

        fault_plan_init(&fp, g_p, rem, DEFAULT_CONNS, seed_state);

        char a_hdr[128];
        int a_len = snprintf(a_hdr, sizeof(a_hdr), "ATTACHED %s\n", s->sid);
        if (fault_send(fd, &fp, a_hdr, a_len) != a_len) {
            session_release(s);
            close(fd);
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
            return NULL;
        }

        log_msg(LOG_INFO, "Conn %u attached to session %s", conn_id, s->sid);
        serve_fetch_loop(fd, s, &fp, &lr, conn_id);

    } else {
        proto_send_err(fd, 400, "BAD REQUEST");
    }

    if (s) {
        session_release(s);
    }
    close(fd);

    pthread_mutex_lock(&g_conn_mu);
    g_active_clients--;
    pthread_mutex_unlock(&g_conn_mu);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability> [opts]\n", argv[0]);
        return EXIT_USAGE;
    }

    int port = atoi(argv[1]);
    const char *topics_root_dir = argv[2];
    g_p = atof(argv[3]);
    if (g_p < 0.0 || g_p > 1.0) {
        fprintf(stderr, "Error: failure_probability must be in range [0.0, 1.0]\n");
        return EXIT_USAGE;
    }

    /* Optional flags */
    for (int i = 4; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "seed", &v)) g_seed = v;
        else if (parse_kv_opt(argv[i], "session_ttl", &v)) g_session_ttl = v;
        else if (parse_kv_opt(argv[i], "max_clients", &v)) g_max_clients = v;
        else if (parse_kv_opt(argv[i], "sendfile", &v)) g_sendfile = v;
        else if (parse_kv_opt(argv[i], "sndbuf", &v)) g_sndbuf = v;
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

    session_table_init();

    pthread_t reaper_tid;
    pthread_create(&reaper_tid, NULL, session_reaper_thread, &g_session_ttl);

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

    log_msg(LOG_INFO, "Part II Case 2 Enhanced Server listening on port %d, p=%.2f, sendfile=%ld",
            port, g_p, g_sendfile);

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
            pthread_mutex_lock(&g_conn_mu);
            g_active_clients--;
            pthread_mutex_unlock(&g_conn_mu);
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
