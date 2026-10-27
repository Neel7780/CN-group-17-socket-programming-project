#define _GNU_SOURCE
#include "common.h"
#include "topics.h"
#include "proto.h"
#include "manifest.h"
#include "workq.h"
#include "checkpoint.h"
#include "metrics.h"

static char g_server_ip[64];
static int g_server_port;
static char g_topic[64];
static char g_output_dir[PATH_MAX];
static char g_topic_dir[PATH_MAX * 2];

static long g_conns = DEFAULT_CONNS;
static long g_ckpt_kb = 64;
static long g_max_retries = DEFAULT_MAX_RETRIES;

static manifest_t g_manifest;
static workq_t *g_workq = NULL;
static ckpt_t *g_ckpt = NULL;
static char g_sid[33];

static pthread_mutex_t g_retry_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_total_retries = 0;

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((uint16_t)g_server_port);
    if (inet_pton(AF_INET, g_server_ip, &saddr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    /* 5-second connection timeout */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int attach_worker_socket(int fd, const char *sid, line_reader_t *lr) {
    char req[128];
    int len = snprintf(req, sizeof(req), "ATTACH %s\n", sid);
    if (send_all(fd, req, len) != len) {
        return -1;
    }

    char line[MAX_LINE];
    ssize_t r = lr_readline(lr, line, sizeof(line));
    if (r <= 0) return -1;

    char ack_sid[33];
    if (sscanf(line, "ATTACHED %32s", ack_sid) == 1 && strcmp(ack_sid, sid) == 0) {
        return 0;
    }
    return -1;
}

typedef struct {
    uint32_t worker_id;
    int initial_fd;
} worker_arg_t;

static void *worker_thread_main(void *arg) {
    worker_arg_t *warg = (worker_arg_t *)arg;
    uint32_t wid = warg->worker_id;
    int fd = warg->initial_fd;
    free(warg);

    line_reader_t lr;
    if (fd >= 0) {
        lr_init(&lr, fd);
    }

    char chunk_buf[65536];

    while (1) {
        batch_task_t task;
        if (!workq_pop(g_workq, &task)) {
            /* Queue empty and closed */
            break;
        }

        /* Ensure we have an active connected socket */
        while (fd < 0) {
            pthread_mutex_lock(&g_retry_mu);
            if (g_total_retries >= (uint32_t)g_max_retries) {
                pthread_mutex_unlock(&g_retry_mu);
                log_msg(LOG_ERR, "Worker %u: Max retries exceeded", wid);
                workq_push_front(g_workq, task.first_idx, task.last_idx, task.offset);
                return NULL;
            }
            g_total_retries++;
            pthread_mutex_unlock(&g_retry_mu);

            usleep(100000); /* 100 ms backoff */
            fd = connect_to_server();
            if (fd >= 0) {
                lr_init(&lr, fd);
                if (attach_worker_socket(fd, g_sid, &lr) != 0) {
                    close(fd);
                    fd = -1;
                } else {
                    log_msg(LOG_INFO, "Worker %u reconnected and attached successfully", wid);
                }
            }
        }

        /* Issue FETCH command */
        char fetch_cmd[256];
        int flen = snprintf(fetch_cmd, sizeof(fetch_cmd), "FETCH %s %u %u %lu\n",
                            g_sid, task.first_idx, task.last_idx, task.offset);
        if (send_all(fd, fetch_cmd, flen) != flen) {
            close(fd);
            fd = -1;
            workq_push_front(g_workq, task.first_idx, task.last_idx, task.offset);
            continue;
        }

        bool batch_success = true;
        uint32_t cur_file_idx = task.first_idx;

        while (cur_file_idx <= task.last_idx) {
            char line[MAX_LINE];
            ssize_t rlen = lr_readline(&lr, line, sizeof(line));
            if (rlen <= 0) {
                batch_success = false;
                break;
            }

            uint32_t f_idx = 0;
            uint64_t f_size = 0;
            uint64_t f_start = 0;
            char f_name[256];
            if (sscanf(line, "FILE %u %lu %lu %255s", &f_idx, &f_size, &f_start, f_name) != 4) {
                batch_success = false;
                break;
            }

            char part_path[16384];
            char final_path[16384];
            snprintf(part_path, sizeof(part_path), "%s/%s.part", g_topic_dir, f_name);
            snprintf(final_path, sizeof(final_path), "%s/%s", g_topic_dir, f_name);

            int out_fd = open(part_path, O_WRONLY | O_CREAT, 0644);
            if (out_fd < 0) {
                log_msg(LOG_ERR, "Worker %u failed to open %s: %s", wid, part_path, strerror(errno));
                batch_success = false;
                break;
            }

            if (f_start > 0) {
                lseek(out_fd, (off_t)f_start, SEEK_SET);
            }

            uint64_t remaining_bytes = f_size - f_start;
            uint64_t transferred = 0;
            bool file_ok = true;

            while (transferred < remaining_bytes) {
                size_t need = remaining_bytes - transferred;
                size_t chunk = (need < sizeof(chunk_buf)) ? need : sizeof(chunk_buf);

                ssize_t n = lr_read_exact(&lr, chunk_buf, chunk);
                if (n <= 0) {
                    file_ok = false;
                    break;
                }

                ssize_t w = write(out_fd, chunk_buf, (size_t)n);
                if (w != n) {
                    file_ok = false;
                    break;
                }
                transferred += (uint64_t)n;
            }

            close(out_fd);

            if (!file_ok) {
                batch_success = false;
                /* Truncate partial file back to starting checkpoint */
                if (truncate(part_path, (off_t)f_start) != 0) {
                    log_msg(LOG_ERR, "Failed to truncate %s: %s", part_path, strerror(errno));
                }
                break;
            }

            /* File successfully completed: rename .part to final */
            rename(part_path, final_path);
            ckpt_mark_complete(g_ckpt, f_idx);
            cur_file_idx++;
        }

        if (batch_success) {
            /* Read BEND delimiter */
            char bend_line[MAX_LINE];
            ssize_t blen = lr_readline(&lr, bend_line, sizeof(bend_line));
            if (blen <= 0 || strncmp(bend_line, "BEND", 4) != 0) {
                batch_success = false;
            } else {
                workq_task_done(g_workq);
            }
        }

        if (!batch_success) {
            log_msg(LOG_INFO, "Worker %u fault/interruption on batch [%u..%u], remaining from %u",
                    wid, task.first_idx, task.last_idx, cur_file_idx);
            close(fd);
            fd = -1;
            if (cur_file_idx <= task.last_idx) {
                workq_push_front(g_workq, cur_file_idx, task.last_idx, 0);
            }
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir> [opts]\n", argv[0]);
        return EXIT_USAGE;
    }

    snprintf(g_server_ip, sizeof(g_server_ip), "%s", argv[1]);
    g_server_port = atoi(argv[2]);
    snprintf(g_topic, sizeof(g_topic), "%s", argv[3]);
    snprintf(g_output_dir, sizeof(g_output_dir), "%s", argv[4]);

    for (int i = 5; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "conns", &v)) g_conns = v;
        else if (parse_kv_opt(argv[i], "ckpt_kb", &v)) g_ckpt_kb = v;
        else if (parse_kv_opt(argv[i], "max_retries", &v)) g_max_retries = v;
        else if (parse_kv_opt(argv[i], "log", &v)) g_log_level = (int)v;
    }

    snprintf(g_topic_dir, sizeof(g_topic_dir), "%s/%s", g_output_dir, g_topic);
    if (ensure_dir_exists(g_topic_dir) != 0) {
        fprintf(stderr, "Error: Could not create output dir %s\n", g_topic_dir);
        return EXIT_IO_ERROR;
    }

    signal(SIGPIPE, SIG_IGN);
    metrics_init("client_metrics.csv");

    uint64_t t_start_total = now_ms();
    int primary_fd = -1;
    line_reader_t primary_lr;
    uint32_t file_count = 0;
    uint64_t total_bytes = 0;

    /* 1. Primary Connection Negotiation */
    long backoff_ms = 100;
    for (int attempt = 0; attempt < g_max_retries; attempt++) {
        primary_fd = connect_to_server();
        if (primary_fd < 0) {
            usleep((useconds_t)backoff_ms * 1000);
            backoff_ms = (backoff_ms * 2 < 2000) ? backoff_ms * 2 : 2000;
            continue;
        }

        lr_init(&primary_lr, primary_fd);

        char get_cmd[128];
        int glen = snprintf(get_cmd, sizeof(get_cmd), "GET %s %ld\n", g_topic, g_conns);
        if (send_all(primary_fd, get_cmd, glen) != glen) {
            close(primary_fd);
            primary_fd = -1;
            usleep((useconds_t)backoff_ms * 1000);
            backoff_ms = (backoff_ms * 2 < 2000) ? backoff_ms * 2 : 2000;
            continue;
        }

        char resp[MAX_LINE];
        ssize_t rlen = lr_readline(&primary_lr, resp, sizeof(resp));
        if (rlen <= 0) {
            close(primary_fd);
            primary_fd = -1;
            usleep(100000);
            continue;
        }

        if (strncmp(resp, "ERR 404", 7) == 0) {
            fprintf(stderr, "Server error: %s\n", resp);
            close(primary_fd);
            return EXIT_TOPIC_NOT_FOUND;
        }

        if (sscanf(resp, "SESSION %32s %u %lu", g_sid, &file_count, &total_bytes) == 3) {
            log_msg(LOG_INFO, "Session established: sid=%s, files=%u, bytes=%lu",
                    g_sid, file_count, total_bytes);

            /* Receive manifest */
            if (manifest_receive(&primary_lr, &g_manifest, file_count, total_bytes) == 0) {
                break; /* Successfully negotiated session and manifest */
            }
        }

        close(primary_fd);
        primary_fd = -1;
        usleep(100000);
    }

    if (primary_fd < 0) {
        fprintf(stderr, "Error: Failed to negotiate session after retries\n");
        return EXIT_RETRY_LIMIT;
    }

    /* 2. Checkpoint and Work Queue */
    g_ckpt = ckpt_create(g_topic_dir, g_topic, g_sid, file_count);
    ckpt_load(g_ckpt);

    g_workq = workq_create();
    manifest_partition_batches(&g_manifest, g_workq, DEFAULT_BATCH_BYTES);

    /* 3. Launch Parallel Worker Threads */
    int n_workers = (int)g_conns;
    if (n_workers < 1) n_workers = 1;
    pthread_t *threads = calloc((size_t)n_workers, sizeof(pthread_t));

    for (int i = 0; i < n_workers; i++) {
        worker_arg_t *warg = malloc(sizeof(worker_arg_t));
        warg->worker_id = (uint32_t)(i + 1);

        if (i == 0) {
            /* Connection 1 reused directly */
            warg->initial_fd = primary_fd;
        } else {
            /* Connections 2..N connect and attach */
            int wfd = connect_to_server();
            if (wfd >= 0) {
                line_reader_t wlr;
                lr_init(&wlr, wfd);
                if (attach_worker_socket(wfd, g_sid, &wlr) != 0) {
                    close(wfd);
                    wfd = -1;
                }
            }
            warg->initial_fd = wfd;
        }
        pthread_create(&threads[i], NULL, worker_thread_main, warg);
    }

    /* 4. Await All Worker Threads */
    for (int i = 0; i < n_workers; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    /* 5. Issue DONE via dedicated final connection if primary was closed */
    int done_fd = connect_to_server();
    if (done_fd >= 0) {
        char done_cmd[128];
        int dlen = snprintf(done_cmd, sizeof(done_cmd), "DONE %s\n", g_sid);
        send_all(done_fd, done_cmd, dlen);
        char bye[MAX_LINE];
        line_reader_t d_lr;
        lr_init(&d_lr, done_fd);
        lr_readline(&d_lr, bye, sizeof(bye));
        close(done_fd);
    }

    /* 6. Verify and Cleanup */
    ckpt_remove(g_ckpt);
    ckpt_free(g_ckpt);
    manifest_free(&g_manifest);
    workq_destroy(g_workq);

    uint64_t dur_total = now_ms() - t_start_total;
    metrics_log_row("client", "COMPLETE", g_sid, 0, g_topic, file_count, total_bytes, dur_total, "SUCCESS");
    metrics_close();

    log_msg(LOG_INFO, "Download of topic '%s' finished in %lu ms (%lu bytes delivered)",
            g_topic, dur_total, total_bytes);
    return EXIT_OK;
}
