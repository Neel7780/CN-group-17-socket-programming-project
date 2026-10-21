#define _GNU_SOURCE
#include "metrics.h"
#include <sys/file.h>

#define METRICS_HEADER \
    "wall_start_ns,wall_end_ns,role,mode,conn_id,topic,files,payload_bytes," \
    "wire_bytes,wait_ms,duration_ms,throughput_mbps,status\n"

typedef struct metrics_item {
    struct metrics_item *next;
    size_t len;
    char line[];
} metrics_item_t;

static int g_fd = -1;
static metrics_mode_t g_mode = METRICS_SYNC;

/* Async logger queue */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static metrics_item_t *g_head = NULL;
static metrics_item_t *g_tail = NULL;
static bool g_stop = false;
static bool g_thread_running = false;
static pthread_t g_thread;

/* Appends s to buf as a CSV field, quoting it if it contains , " or a newline. */
static size_t csv_field(char *buf, size_t cap, const char *s) {
    if (!s) s = "";
    bool quote = strpbrk(s, ",\"\r\n") != NULL;
    size_t n = 0;
    if (quote && n + 1 < cap) buf[n++] = '"';
    for (; *s && n + 3 < cap; s++) {
        if (*s == '"') buf[n++] = '"';
        buf[n++] = *s;
    }
    if (quote && n + 1 < cap) buf[n++] = '"';
    buf[n] = '\0';
    return n;
}

static int format_row(char *buf, size_t cap, const metrics_row_t *r) {
    char role[32], mode[96], topic[160], status[64];
    csv_field(role, sizeof(role), r->role);
    csv_field(mode, sizeof(mode), r->mode);
    csv_field(topic, sizeof(topic), r->topic);
    csv_field(status, sizeof(status), r->status);

    double secs = (double)r->duration_ns / 1e9;
    double mbps = secs > 0 ? (double)r->payload_bytes * 8.0 / secs / 1e6 : 0.0;
    int len = snprintf(buf, cap,
                       "%" PRIu64 ",%" PRIu64 ",%s,%s,%" PRIu64 ",%s,%" PRIu32 ",%" PRIu64
                       ",%" PRIu64 ",%.3f,%.3f,%.3f,%s\n",
                       r->wall_start_ns, r->wall_start_ns + r->duration_ns, role, mode,
                       r->conn_id, topic, r->files, r->payload_bytes, r->wire_bytes,
                       (double)r->wait_ns / 1e6, (double)r->duration_ns / 1e6, mbps, status);
    return (len < 0 || (size_t)len >= cap) ? -1 : len;
}

static void write_line(const char *line, size_t len) {
    if (write_all(g_fd, line, len) < 0) {
        log_msg(LOG_WARN, "metrics: write failed: %s", strerror(errno));
    }
}

static void *logger_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_head && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
        if (!g_head && g_stop) break;

        /* Take the whole batch and write it without holding the lock */
        metrics_item_t *batch = g_head;
        g_head = g_tail = NULL;
        pthread_mutex_unlock(&g_mu);
        while (batch) {
            metrics_item_t *next = batch->next;
            write_line(batch->line, batch->len);
            free(batch);
            batch = next;
        }
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int metrics_open(const char *csv_path, metrics_mode_t mode) {
    if (!csv_path || csv_path[0] == '\0') return -1;

    int fd = open(csv_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        log_msg(LOG_WARN, "metrics: cannot open %s: %s", csv_path, strerror(errno));
        return -1;
    }

    /* Several processes may open the file at once; only one writes the header */
    struct stat st;
    flock(fd, LOCK_EX);
    if (fstat(fd, &st) == 0 && st.st_size == 0) {
        write_all(fd, METRICS_HEADER, strlen(METRICS_HEADER));
    }
    flock(fd, LOCK_UN);

    g_fd = fd;
    g_mode = mode;
    g_stop = false;
    if (mode == METRICS_ASYNC) {
        if (pthread_create(&g_thread, NULL, logger_thread, NULL) != 0) {
            log_msg(LOG_WARN, "metrics: logger thread failed, writing synchronously");
            g_mode = METRICS_SYNC;
        } else {
            g_thread_running = true;
        }
    }
    return 0;
}

void metrics_row(const metrics_row_t *row) {
    if (g_fd < 0 || !row) return;

    char line[512];
    int len = format_row(line, sizeof(line), row);
    if (len < 0) {
        log_msg(LOG_WARN, "metrics: row too long, dropped");
        return;
    }

    if (g_mode == METRICS_SYNC) {
        write_line(line, (size_t)len);
        return;
    }

    metrics_item_t *item = malloc(sizeof(*item) + (size_t)len + 1);
    if (!item) return;
    item->next = NULL;
    item->len = (size_t)len;
    memcpy(item->line, line, (size_t)len + 1);

    pthread_mutex_lock(&g_mu);
    if (g_tail) g_tail->next = item;
    else g_head = item;
    g_tail = item;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

void metrics_close(void) {
    if (g_thread_running) {
        pthread_mutex_lock(&g_mu);
        g_stop = true;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu);
        pthread_join(g_thread, NULL);
        g_thread_running = false;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}
