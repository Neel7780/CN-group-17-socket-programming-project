#define _GNU_SOURCE
#include "common.h"
#include "topics.h"
#include "proto.h"
#include "checkpoint.h"
#include "metrics.h"

static char g_server_ip[64];
static int g_server_port;
static char g_topic[64];
static char g_output_dir[PATH_MAX];
static char g_topic_dir[PATH_MAX * 2];

static long g_ckpt_kb = 64;                       /* intra-file checkpoint interval */
static long g_max_retries = DEFAULT_MAX_RETRIES;
static long g_backoff_ms = DEFAULT_BACKOFF_MS;
static long g_backoff_max_ms = DEFAULT_BACKOFF_MAX_MS;

static ckpt_t *g_ckpt = NULL;

/* Transfer statistics, reported in the metrics row. */
static uint64_t g_total_received = 0;   /* all bytes received across attempts */
static uint64_t g_bytes_redundant = 0;  /* bytes received but not delivered to disk */
static uint32_t g_attempts = 0;
static bool g_used_resume = false;

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

    sock_disable_sigpipe(fd);

    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Sanitises a file name received from the server so that it can only ever
 * be written inside the topic directory (Design Document Section 7, path
 * traversal safety).
 */
static bool safe_filename(const char *name) {
    if (!name || name[0] == '\0') return false;
    if (strchr(name, '/') != NULL) return false;
    if (strstr(name, "..") != NULL) return false;
    if (name[0] == '.') return false;
    return true;
}

static void part_path_for(char *out, size_t outlen, const char *name) {
    snprintf(out, outlen, "%s/%s.part", g_topic_dir, name);
}

static void final_path_for(char *out, size_t outlen, const char *name) {
    snprintf(out, outlen, "%s/%s", g_topic_dir, name);
}

/*
 * Receives the file stream, writing each file to "<name>.part" and
 * renaming it to "<name>" once complete. The checkpoint is updated after
 * every completed file and every ckpt_kb bytes within a partial file.
 *
 * Returns 0 when the END delimiter was received, -1 on a connection
 * failure (the caller then reconnects and resumes).
 */
static int recv_files(line_reader_t *lr, uint32_t expected_count, uint64_t expected_total) {
    char line[MAX_LINE];
    uint64_t ckpt_interval = (uint64_t)g_ckpt_kb * 1024;
    if (ckpt_interval == 0) ckpt_interval = DEFAULT_CKPT_INTERVAL;

    char chunk_buf[65536];

    while (1) {
        ssize_t rlen = lr_readline(lr, line, sizeof(line));
        if (rlen <= 0) {
            log_msg(LOG_INFO, "Connection lost while receiving file stream");
            return -1;
        }

        if (strncmp(line, "END ", 4) == 0) {
            uint32_t end_count = 0;
            uint64_t end_total = 0;
            if (sscanf(line, "END %u %" SCNu64, &end_count, &end_total) != 2) {
                log_msg(LOG_ERR, "Malformed END line: %s", line);
                return -1;
            }
            if (end_count != expected_count || end_total != expected_total) {
                log_msg(LOG_ERR, "END mismatch: got %u files / %" PRIu64 " bytes, expected %u / %" PRIu64,
                        end_count, end_total, expected_count, expected_total);
                return -1;
            }
            log_msg(LOG_INFO, "END received: %u files, %" PRIu64 " bytes", end_count, end_total);
            return 0;
        }

        if (strncmp(line, "ERR ", 4) == 0) {
            log_msg(LOG_ERR, "Server error: %s", line);
            return -1;
        }

        if (strncmp(line, "FILE ", 5) != 0) {
            log_msg(LOG_ERR, "Unexpected line from server: %s", line);
            return -1;
        }

        uint32_t f_idx = 0;
        uint64_t f_size = 0;
        uint64_t f_start = 0;
        char f_name[256];
        /* The name is the final token; use %n to find where it starts. */
        int name_pos = 0;
        if (sscanf(line, "FILE %u %" SCNu64 " %" SCNu64 " %n", &f_idx, &f_size, &f_start, &name_pos) < 3
            || name_pos <= 0) {
            log_msg(LOG_ERR, "Malformed FILE header: %s", line);
            return -1;
        }
        /* Skip the single space before the name. */
        char *name_ptr = line + name_pos;
        while (*name_ptr == ' ') name_ptr++;
        snprintf(f_name, sizeof(f_name), "%s", name_ptr);

        if (!safe_filename(f_name)) {
            log_msg(LOG_ERR, "Rejected unsafe filename: %s", f_name);
            return -1;
        }
        if (f_idx >= expected_count) {
            log_msg(LOG_ERR, "File index %u out of range", f_idx);
            return -1;
        }
        if (f_start > f_size) {
            log_msg(LOG_ERR, "Invalid start offset %" PRIu64 " for file %u", f_start, f_idx);
            return -1;
        }

        char part_path[PATH_MAX * 2];
        char final_path[PATH_MAX * 2];
        part_path_for(part_path, sizeof(part_path), f_name);
        final_path_for(final_path, sizeof(final_path), f_name);

        int out_fd = open(part_path, O_WRONLY | O_CREAT, 0644);
        if (out_fd < 0) {
            log_msg(LOG_ERR, "Failed to open %s: %s", part_path, strerror(errno));
            return -1;
        }

        /* Position the write cursor at the resume offset. */
        if (f_start > 0) {
            if (lseek(out_fd, (off_t)f_start, SEEK_SET) == (off_t)-1) {
                close(out_fd);
                return -1;
            }
        }

        log_msg(LOG_DEBUG, "RX FILE idx=%u size=%" PRIu64 " start=%" PRIu64 " name=%s",
                f_idx, f_size, f_start, f_name);

        uint64_t remaining = f_size - f_start;
        uint64_t transferred = 0;
        uint64_t since_ckpt = 0;
        bool file_ok = true;

        while (transferred < remaining) {
            size_t need = (size_t)(remaining - transferred);
            size_t chunk = (need < sizeof(chunk_buf)) ? need : sizeof(chunk_buf);

            ssize_t n = lr_read_exact(lr, chunk_buf, chunk);
            if (n <= 0) {
                file_ok = false;
                break;
            }

            ssize_t w = write(out_fd, chunk_buf, (size_t)n);
            if (w != n) {
                /*
                 * A partial write still delivered those bytes, so they are
                 * counted as received. The file is rolled back to the last
                 * checkpoint further down.
                 */
                transferred += (uint64_t)w;
                g_total_received += (uint64_t)w;
                file_ok = false;
                break;
            }

            transferred += (uint64_t)n;
            g_total_received += (uint64_t)n;
            since_ckpt += (uint64_t)n;

            /* Periodic intra-file checkpoint: make the bytes durable, then
             * record the new offset. */
            if (since_ckpt >= ckpt_interval) {
                if (fsync(out_fd) != 0) {
                    log_msg(LOG_WARN, "fsync failed on %s: %s", part_path, strerror(errno));
                }
                ckpt_set_progress(g_ckpt, f_idx, f_start + transferred);
                since_ckpt = 0;
            }
        }

        close(out_fd);

        if (!file_ok) {
            /*
             * Roll the partial file back to the last durable checkpoint so
             * that no uncheckpointed bytes are mistaken for delivered data.
             * If the file was never checkpointed, drop it entirely.
             */
            uint32_t ck_idx = g_ckpt->cur_idx;
            uint64_t ck_off = g_ckpt->cur_off;
            bool this_file_checkpointed = (f_idx == ck_idx) && (ck_off > 0) && (ck_off <= f_start + transferred);
            if (this_file_checkpointed) {
                truncate(part_path, (off_t)ck_off);
            } else {
                unlink(part_path);
            }
            log_msg(LOG_INFO, "Transfer interrupted during file %u (%s)", f_idx, f_name);
            return -1;
        }

        /* File complete: rename .part to the final name. */
        if (rename(part_path, final_path) != 0) {
            log_msg(LOG_ERR, "Failed to rename %s: %s", part_path, strerror(errno));
            return -1;
        }
        ckpt_mark_complete(g_ckpt, f_idx);
    }
}

/*
 * Removes every partially received file and the checkpoint, so that a fresh
 * GET starts from a clean slate (used when a resume is rejected).
 */
static void discard_partial_state(void) {
    if (!g_ckpt) return;
    char part_path[PATH_MAX * 2];
    for (uint32_t i = 0; i < g_ckpt->file_count; i++) {
        /* The file name is not stored in the checkpoint; scan the directory
         * for leftovers instead. */
        (void)i;
    }
    DIR *d = opendir(g_topic_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t nlen = strlen(de->d_name);
            if (nlen > 5 && strcmp(de->d_name + nlen - 5, ".part") == 0) {
                snprintf(part_path, sizeof(part_path), "%s/%s", g_topic_dir, de->d_name);
                unlink(part_path);
            }
        }
        closedir(d);
    }
    ckpt_remove(g_ckpt);
}

/*
 * Trims the partial file of the checkpointed file back to the durable
 * offset, discarding bytes that were received but never checkpointed.
 */
static void rollback_partial_file(void) {
    if (!g_ckpt) return;
    if (g_ckpt->cur_idx >= g_ckpt->file_count) return;
    if (g_ckpt->cur_off == 0) return;

    DIR *d = opendir(g_topic_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen > 5 && strcmp(de->d_name + nlen - 5, ".part") == 0) {
            char part_path[PATH_MAX * 2];
            snprintf(part_path, sizeof(part_path), "%s/%s", g_topic_dir, de->d_name);
            struct stat st;
            if (stat(part_path, &st) == 0 && (uint64_t)st.st_size > g_ckpt->cur_off) {
                truncate(part_path, (off_t)g_ckpt->cur_off);
                log_msg(LOG_INFO, "Rolled back partial file %s to %" PRIu64 " bytes",
                        de->d_name, g_ckpt->cur_off);
            }
        }
    }
    closedir(d);
}

/*
 * Sends DONE to release the session state on the server. This is best
 * effort: if it fails the transfer is still complete and the server reaper
 * reclaims the orphaned session.
 */
static void send_done(const char *sid) {
    int fd = connect_to_server();
    if (fd < 0) return;

    char req[128];
    int len = snprintf(req, sizeof(req), "DONE %s\n", sid);
    if (send_all(fd, req, (size_t)len) == len) {
        line_reader_t lr;
        lr_init(&lr, fd);
        char resp[MAX_LINE];
        if (lr_readline(&lr, resp, sizeof(resp)) > 0) {
            log_msg(LOG_INFO, "Server acknowledged completion: %s", resp);
        }
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir> [opts]\n", argv[0]);
        fprintf(stderr, "Options: ckpt_kb=, max_retries=, backoff_ms=, log=0..3\n");
        return EXIT_USAGE;
    }

    snprintf(g_server_ip, sizeof(g_server_ip), "%s", argv[1]);
    g_server_port = atoi(argv[2]);
    snprintf(g_topic, sizeof(g_topic), "%s", argv[3]);
    snprintf(g_output_dir, sizeof(g_output_dir), "%s", argv[4]);

    for (int i = 5; i < argc; i++) {
        long v;
        if (parse_kv_opt(argv[i], "ckpt_kb", &v)) g_ckpt_kb = v;
        else if (parse_kv_opt(argv[i], "max_retries", &v)) g_max_retries = v;
        else if (parse_kv_opt(argv[i], "backoff_ms", &v)) g_backoff_ms = v;
        else if (parse_kv_opt(argv[i], "backoff_max_ms", &v)) g_backoff_max_ms = v;
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
    uint32_t file_count = 0;
    uint64_t total_bytes = 0;
    char sid[33];
    memset(sid, 0, sizeof(sid));

    /*
     * Attempt loop. The first attempt issues GET; every later attempt
     * issues RESUME with the client's last durable checkpoint. If a resume
     * is rejected the client falls back to a clean GET.
     */
    bool have_session = false;
    bool fallback_to_get = false;
    int attempt = 0;
    long backoff = g_backoff_ms;

    for (attempt = 0; attempt < g_max_retries; attempt++) {
        int fd = connect_to_server();
        if (fd < 0) {
            usleep((useconds_t)g_backoff_ms * 1000);
            continue;
        }
        g_attempts++;

        line_reader_t lr;
        lr_init(&lr, fd);

        bool want_resume = have_session && !fallback_to_get && g_ckpt != NULL &&
                           (g_ckpt->cur_idx > 0 || g_ckpt->done > 0);

        char req[256];
        int rlen;
        if (want_resume) {
            rlen = snprintf(req, sizeof(req), "RESUME %s %s %u %" PRIu64 "\n",
                            g_ckpt->sid, g_topic, g_ckpt->cur_idx, g_ckpt->cur_off);
            g_used_resume = true;
            log_msg(LOG_INFO, "Attempt %d: RESUME sid=%s idx=%u off=%" PRIu64,
                    attempt + 1, g_ckpt->sid, g_ckpt->cur_idx, g_ckpt->cur_off);
        } else {
            rlen = snprintf(req, sizeof(req), "GET %s\n", g_topic);
            g_used_resume = false;
            log_msg(LOG_INFO, "Attempt %d: GET %s", attempt + 1, g_topic);
        }

        if (send_all(fd, req, (size_t)rlen) != rlen) {
            close(fd);
            usleep((useconds_t)backoff * 1000);
            backoff = (backoff * 2 < g_backoff_max_ms) ? backoff * 2 : g_backoff_max_ms;
            continue;
        }

        char resp[MAX_LINE];
        ssize_t resplen = lr_readline(&lr, resp, sizeof(resp));
        if (resplen <= 0) {
            close(fd);
            usleep((useconds_t)backoff * 1000);
            backoff = (backoff * 2 < g_backoff_max_ms) ? backoff * 2 : g_backoff_max_ms;
            continue;
        }

        /* An ERR response to a RESUME means the checkpoint is unusable. */
        if (strncmp(resp, "ERR ", 4) == 0) {
            int code = 0;
            sscanf(resp, "ERR %d", &code);
            log_msg(LOG_INFO, "Server rejected request: %s", resp);
            if (code == PROTO_ERR_NOT_FOUND) {
                fprintf(stderr, "Error: topic '%s' not found on server\n", g_topic);
                close(fd);
                return EXIT_TOPIC_NOT_FOUND;
            }
            if (want_resume) {
                /* Fall back to a clean GET. */
                log_msg(LOG_INFO, "Resume rejected, discarding checkpoint and restarting from scratch");
                discard_partial_state();
                if (g_ckpt) {
                    ckpt_free(g_ckpt);
                    g_ckpt = NULL;
                }
                have_session = false;
                fallback_to_get = true;
                close(fd);
                usleep((useconds_t)backoff * 1000);
                backoff = (backoff * 2 < g_backoff_max_ms) ? backoff * 2 : g_backoff_max_ms;
                continue;
            }
            fprintf(stderr, "Server error: %s\n", resp);
            close(fd);
            return EXIT_IO_ERROR;
        }

        if (strncmp(resp, "SESSION ", 8) == 0) {
            if (sscanf(resp, "SESSION %32s %u %" SCNu64, sid, &file_count, &total_bytes) != 3) {
                close(fd);
                return EXIT_IO_ERROR;
            }
            have_session = true;
            fallback_to_get = false;

            /* Create (or reload) the durable checkpoint for this session. */
            if (g_ckpt) ckpt_free(g_ckpt);
            g_ckpt = ckpt_create(g_topic_dir, g_topic, sid, file_count);
            if (!g_ckpt) {
                close(fd);
                return EXIT_IO_ERROR;
            }
            ckpt_load(g_ckpt);
            ckpt_save_atomic(g_ckpt);

            /* A fresh session invalidates any previous partial files. */
            discard_partial_state();
            ckpt_save_atomic(g_ckpt);

            log_msg(LOG_INFO, "Session %s: %u files, %" PRIu64 " bytes", sid, file_count, total_bytes);
        } else if (strncmp(resp, "RESUMED ", 8) == 0) {
            uint32_t ridx = 0;
            uint64_t roff = 0, rrem = 0;
            if (sscanf(resp, "RESUMED %u %" SCNu64 " %" SCNu64, &ridx, &roff, &rrem) != 3) {
                close(fd);
                return EXIT_IO_ERROR;
            }
            log_msg(LOG_INFO, "Server resumed at file %u offset %" PRIu64 " (%" PRIu64 " bytes remaining)",
                    ridx, roff, rrem);
            /* Discard any bytes that were received but not checkpointed. */
            rollback_partial_file();
        } else {
            log_msg(LOG_ERR, "Unexpected response: %s", resp);
            close(fd);
            return EXIT_IO_ERROR;
        }

        int rc = recv_files(&lr, file_count, total_bytes);
        close(fd);

        if (rc == 0) {
            /*
             * Transfer complete. The useful payload is exactly the topic
             * size; anything received beyond that was redundant.
             */
            uint64_t useful = total_bytes;
            g_bytes_redundant = (g_total_received > useful) ? (g_total_received - useful) : 0;
            uint64_t dur_total = now_ms() - t_start_total;
            metrics_log_row("client", "COMPLETE", sid, 0, g_topic, file_count, g_total_received,
                            dur_total, g_used_resume ? "SUCCESS_RESUMED" : "SUCCESS");
            log_msg(LOG_INFO, "Download of '%s' finished in %" PRIu64 " ms (%" PRIu64 " useful bytes, "
                    "%" PRIu64 " redundant, %u attempts)",
                    g_topic, dur_total, useful, g_bytes_redundant, g_attempts);

            /* Best-effort session cleanup. */
            send_done(sid);

            ckpt_remove(g_ckpt);
            ckpt_free(g_ckpt);
            g_ckpt = NULL;
            metrics_close();
            return EXIT_OK;
        }

        /* The connection failed: keep the checkpoint and resume on retry. */
        if (g_ckpt) {
            ckpt_save_atomic(g_ckpt);
        }

        usleep((useconds_t)backoff * 1000);
        backoff = (backoff * 2 < g_backoff_max_ms) ? backoff * 2 : g_backoff_max_ms;
    }

    fprintf(stderr, "Error: transfer did not complete after %d attempts\n", g_attempts);
    metrics_log_row("client", "FAILED", sid, 0, g_topic, file_count, g_total_received,
                    now_ms() - t_start_total, "RETRY_LIMIT");
    if (g_ckpt) {
        ckpt_free(g_ckpt);
        g_ckpt = NULL;
    }
    metrics_close();
    return EXIT_RETRY_LIMIT;
}
