#define _GNU_SOURCE
#include "session.h"

#define BUCKETS 256
static session_t *g_table[BUCKETS];
static pthread_mutex_t g_table_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_reaper_running = true;

static uint8_t hash_sid(const char *sid) {
    uint32_t h = 5381;
    for (int i = 0; sid[i]; i++) {
        h = ((h << 5) + h) + (uint8_t)sid[i];
    }
    return (uint8_t)(h % BUCKETS);
}

static void gen_random_sid(char sid_out[33]) {
    uint8_t bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, bytes, sizeof(bytes)) != sizeof(bytes)) {
            for (int i = 0; i < 16; i++) bytes[i] = (uint8_t)rand();
        }
        close(fd);
    } else {
        for (int i = 0; i < 16; i++) bytes[i] = (uint8_t)rand();
    }
    for (int i = 0; i < 16; i++) {
        sprintf(sid_out + (i * 2), "%02x", bytes[i]);
    }
    sid_out[32] = '\0';
}

void session_table_init(void) {
    pthread_mutex_lock(&g_table_mu);
    memset(g_table, 0, sizeof(g_table));
    pthread_mutex_unlock(&g_table_mu);
}

session_t *session_create(const topic_t *topic) {
    session_t *s = calloc(1, sizeof(session_t));
    if (!s) return NULL;

    gen_random_sid(s->sid);
    s->topic = topic;
    pthread_mutex_init(&s->mu, NULL);
    s->hw_pos = 0;
    s->bytes_sent = 0;
    s->attempts = 1;
    s->generation = 1;
    s->refs = 1;
    s->last_seen = time(NULL);

    uint8_t b = hash_sid(s->sid);
    pthread_mutex_lock(&g_table_mu);
    s->next = g_table[b];
    g_table[b] = s;
    pthread_mutex_unlock(&g_table_mu);

    log_msg(LOG_INFO, "Session created: sid=%s, topic=%s", s->sid, topic->key);
    return s;
}

session_t *session_lookup(const char *sid) {
    if (!sid) return NULL;
    uint8_t b = hash_sid(sid);
    pthread_mutex_lock(&g_table_mu);
    session_t *curr = g_table[b];
    while (curr) {
        if (strcmp(curr->sid, sid) == 0) {
            pthread_mutex_lock(&curr->mu);
            curr->refs++;
            curr->last_seen = time(NULL);
            pthread_mutex_unlock(&curr->mu);
            pthread_mutex_unlock(&g_table_mu);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_table_mu);
    return NULL;
}

void session_release(session_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    if (s->refs > 0) {
        s->refs--;
    }
    s->last_seen = time(NULL);
    pthread_mutex_unlock(&s->mu);
}

void session_destroy(const char *sid) {
    if (!sid) return;
    uint8_t b = hash_sid(sid);
    pthread_mutex_lock(&g_table_mu);
    session_t **curr = &g_table[b];
    while (*curr) {
        session_t *entry = *curr;
        if (strcmp(entry->sid, sid) == 0) {
            *curr = entry->next;
            pthread_mutex_unlock(&g_table_mu);
            pthread_mutex_destroy(&entry->mu);
            free(entry);
            log_msg(LOG_INFO, "Session destroyed: sid=%s", sid);
            return;
        }
        curr = &entry->next;
    }
    pthread_mutex_unlock(&g_table_mu);
}

void session_update_activity(session_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    s->last_seen = time(NULL);
    pthread_mutex_unlock(&s->mu);
}

int session_validate_resume(session_t *s, const char *topic, uint32_t file_idx, uint64_t offset, uint64_t *rem_out) {
    if (!s || !topic) return -1;

    pthread_mutex_lock(&s->mu);
    /* 1. Topic match */
    if (strcasecmp(s->topic->key, topic) != 0) {
        pthread_mutex_unlock(&s->mu);
        return 409; /* TOPIC MISMATCH */
    }

    /* 2. Bounds check */
    if (file_idx > s->topic->file_count) {
        pthread_mutex_unlock(&s->mu);
        return 416; /* BAD CHECKPOINT */
    }
    if (file_idx < s->topic->file_count) {
        if (offset > s->topic->files[file_idx].size) {
            pthread_mutex_unlock(&s->mu);
            return 416; /* BAD CHECKPOINT */
        }
    } else {
        if (offset != 0) {
            pthread_mutex_unlock(&s->mu);
            return 416; /* BAD CHECKPOINT */
        }
    }

    /* 3. High-water mark check */
    uint64_t req_pos = topic_pos(s->topic, file_idx, offset);
    if (req_pos > s->hw_pos) {
        pthread_mutex_unlock(&s->mu);
        return 416; /* BAD CHECKPOINT */
    }

    s->generation++;
    s->attempts++;
    s->last_seen = time(NULL);
    if (rem_out) {
        *rem_out = (s->topic->total_bytes > req_pos) ? (s->topic->total_bytes - req_pos) : 0;
    }
    pthread_mutex_unlock(&s->mu);
    return 0;
}

void *session_reaper_thread(void *arg) {
    long ttl = arg ? *(long *)arg : DEFAULT_SESSION_TTL;
    while (g_reaper_running) {
        sleep(5);
        if (!g_reaper_running) break;

        time_t now = time(NULL);
        pthread_mutex_lock(&g_table_mu);
        for (int b = 0; b < BUCKETS; b++) {
            session_t **curr = &g_table[b];
            while (*curr) {
                session_t *entry = *curr;
                pthread_mutex_lock(&entry->mu);
                if (entry->refs == 0 && (now - entry->last_seen) > ttl) {
                    *curr = entry->next;
                    pthread_mutex_unlock(&entry->mu);
                    pthread_mutex_destroy(&entry->mu);
                    log_msg(LOG_INFO, "Reaper purged expired session: sid=%s", entry->sid);
                    free(entry);
                } else {
                    pthread_mutex_unlock(&entry->mu);
                    curr = &entry->next;
                }
            }
        }
        pthread_mutex_unlock(&g_table_mu);
    }
    return NULL;
}

void session_reaper_stop(void) {
    g_reaper_running = false;
}
