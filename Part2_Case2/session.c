#define _GNU_SOURCE
#include "session.h"

#define BUCKETS 256

static session_t *g_table[BUCKETS];
static pthread_mutex_t g_table_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_reaper_running = true;
static long g_session_ttl = DEFAULT_SESSION_TTL;

static uint8_t hash_sid(const char *sid) {
    uint32_t h = 5381;
    for (int i = 0; sid[i]; i++) {
        h = ((h << 5) + h) + (uint8_t)sid[i];
    }
    return (uint8_t)(h % BUCKETS);
}

static void gen_random_sid(char sid_out[33]) {
    uint8_t bytes[16];
    bool ok = false;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        ok = (n == (ssize_t)sizeof(bytes));
    }
    if (!ok) {
        for (int i = 0; i < 16; i++) {
            bytes[i] = (uint8_t)(rand() & 0xFF);
        }
    }
    for (int i = 0; i < 16; i++) {
        snprintf(sid_out + (i * 2), 3, "%02x", bytes[i]);
    }
    sid_out[32] = '\0';
}

/*
 * Frees a session once no thread can legally touch it any more.
 * lrefs counts: 1 while the session is linked in the table, plus one for
 * every outstanding session_lookup() reference. A session whose lrefs has
 * dropped to zero is unreachable and may be destroyed.
 */
static void session_free(session_t *s) {
    if (!s) return;
    pthread_mutex_destroy(&s->mu);
    free(s);
}

void session_table_init(long ttl) {
    pthread_mutex_lock(&g_table_mu);
    memset(g_table, 0, sizeof(g_table));
    pthread_mutex_unlock(&g_table_mu);
    g_session_ttl = (ttl > 0) ? ttl : DEFAULT_SESSION_TTL;
}

void session_table_set_ttl(long ttl) {
    if (ttl > 0) g_session_ttl = ttl;
}

long session_table_get_ttl(void) {
    return g_session_ttl;
}

session_t *session_create(const topic_t *topic) {
    if (!topic) return NULL;

    session_t *s = calloc(1, sizeof(session_t));
    if (!s) return NULL;

    pthread_mutex_init(&s->mu, NULL);
    s->topic = topic;
    s->hw_pos = 0;
    s->bytes_sent = 0;
    s->attempts = 1;
    s->generation = 1;
    s->refs = 1;      /* the connection that issued the GET is active */
    s->lrefs = 2;     /* one for the table, one for the returned reference */
    s->last_seen = time(NULL);

    pthread_mutex_lock(&g_table_mu);
    /* Guarantee sid uniqueness within the table. */
    for (int tries = 0; tries < 16; tries++) {
        gen_random_sid(s->sid);
        uint8_t b = hash_sid(s->sid);
        bool clash = false;
        for (session_t *c = g_table[b]; c; c = c->next) {
            if (strcmp(c->sid, s->sid) == 0) { clash = true; break; }
        }
        if (!clash) {
            s->next = g_table[b];
            g_table[b] = s;
            break;
        }
        if (tries == 15) {
            pthread_mutex_unlock(&g_table_mu);
            session_free(s);
            return NULL;
        }
    }
    pthread_mutex_unlock(&g_table_mu);

    log_msg(LOG_INFO, "Session created: sid=%s, topic=%s", s->sid, topic->key);
    return s;
}

session_t *session_lookup(const char *sid) {
    if (!sid || !*sid) return NULL;

    session_t *found = NULL;
    pthread_mutex_lock(&g_table_mu);
    uint8_t b = hash_sid(sid);
    for (session_t *c = g_table[b]; c; c = c->next) {
        if (strcmp(c->sid, sid) == 0) {
            c->lrefs++;      /* keep the descriptor alive past table removal */
            found = c;
            break;
        }
    }
    pthread_mutex_unlock(&g_table_mu);

    if (!found) return NULL;

    pthread_mutex_lock(&found->mu);
    found->refs++;
    found->last_seen = time(NULL);
    pthread_mutex_unlock(&found->mu);
    return found;
}

/*
 * Drops a reference taken by session_create() or session_lookup().
 *
 * `refs` counts active connections and `lrefs` counts the lifetime
 * references (one per outstanding pointer to the descriptor plus one held
 * by the table). They are always acquired and released together, so both
 * counters are decremented here.
 *
 * Note that refs reaching zero does NOT remove the session from the table:
 * the session must outlive a dropped connection so that the client can
 * reconnect and resume. Only the TTL reaper or an explicit DONE removes
 * it from the table.
 */
void session_release(session_t *s) {
    if (!s) return;

    pthread_mutex_lock(&s->mu);
    if (s->refs > 0) s->refs--;
    s->last_seen = time(NULL);
    pthread_mutex_unlock(&s->mu);

    /* Drop the lifetime reference. */
    bool destroy = false;
    pthread_mutex_lock(&g_table_mu);
    if (s->lrefs > 0) {
        s->lrefs--;
        if (s->lrefs == 0) destroy = true;
    }
    pthread_mutex_unlock(&g_table_mu);

    if (destroy) {
        log_msg(LOG_INFO, "Session reclaimed: sid=%s", s->sid);
        session_free(s);
    }
}

void session_destroy(const char *sid) {
    if (!sid || !*sid) return;

    session_t *detached = NULL;
    pthread_mutex_lock(&g_table_mu);
    uint8_t b = hash_sid(sid);
    session_t **curr = &g_table[b];
    while (*curr) {
        session_t *entry = *curr;
        if (strcmp(entry->sid, sid) == 0) {
            *curr = entry->next;
            entry->next = NULL;
            detached = entry;
            break;
        }
        curr = &entry->next;
    }
    if (detached) {
        /* Drop the table's own reference. */
        bool destroy = false;
        if (detached->lrefs > 0) {
            detached->lrefs--;
            if (detached->lrefs == 0) destroy = true;
        }
        pthread_mutex_unlock(&g_table_mu);
        log_msg(LOG_INFO, "Session destroyed: sid=%s", sid);
        if (destroy) session_free(detached);
    } else {
        pthread_mutex_unlock(&g_table_mu);
    }
}

void session_update_activity(session_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    s->last_seen = time(NULL);
    pthread_mutex_unlock(&s->mu);
}

uint32_t session_generation(session_t *s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->mu);
    uint32_t g = s->generation;
    pthread_mutex_unlock(&s->mu);
    return g;
}

void session_update_hw(session_t *s, uint64_t pos) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    if (pos > s->hw_pos) s->hw_pos = pos;
    s->last_seen = time(NULL);
    pthread_mutex_unlock(&s->mu);
}

void session_add_bytes_sent(session_t *s, uint64_t bytes) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    s->bytes_sent += bytes;
    pthread_mutex_unlock(&s->mu);
}

/*
 * Design Document Section 5.4 -- checkpoint validation.
 *
 * The client's checkpoint is expressed as (file index, offset within that
 * file). Comparing raw tuples is ambiguous at file boundaries, so the tuple
 * is first projected onto the continuous payload position
 *
 *     pos(i, o) = topic->files[i].pos + o
 *
 * and compared against the server's high-water mark, which is an upper
 * bound on what the client could possibly have received.
 */
int session_validate_resume(session_t *s, const char *topic, uint32_t file_idx,
                             uint64_t offset, uint64_t *rem_out) {
    if (!s || !topic) return PROTO_ERR_SESSION_EXPIRED;

    pthread_mutex_lock(&s->mu);

    /* Rule 2: the requested topic must match the session's bound topic. */
    if (strcasecmp(s->topic->key, topic) != 0) {
        pthread_mutex_unlock(&s->mu);
        log_msg(LOG_INFO, "RESUME rejected: topic mismatch (session=%s, requested=%s)",
                s->topic->key, topic);
        return PROTO_ERR_TOPIC_MISMATCH;
    }

    /* Rule 3a: index and offset bounds. */
    if (file_idx > s->topic->file_count) {
        pthread_mutex_unlock(&s->mu);
        log_msg(LOG_INFO, "RESUME rejected: file index %u out of range", file_idx);
        return PROTO_ERR_BAD_CHECKPOINT;
    }
    if (file_idx < s->topic->file_count) {
        if (offset > s->topic->files[file_idx].size) {
            pthread_mutex_unlock(&s->mu);
            log_msg(LOG_INFO, "RESUME rejected: offset %" PRIu64 " exceeds file %u size",
                    offset, file_idx);
            return PROTO_ERR_BAD_CHECKPOINT;
        }
    } else {
        /* file_idx == file_count means every file is already on disk. */
        if (offset != 0) {
            pthread_mutex_unlock(&s->mu);
            log_msg(LOG_INFO, "RESUME rejected: offset must be 0 at end of topic");
            return PROTO_ERR_BAD_CHECKPOINT;
        }
    }

    /* Rule 3b: the checkpoint must not be ahead of the high-water mark. */
    uint64_t req_pos = topic_pos(s->topic, file_idx, offset);
    if (req_pos > s->hw_pos) {
        pthread_mutex_unlock(&s->mu);
        log_msg(LOG_INFO, "RESUME rejected: checkpoint pos %" PRIu64 " exceeds high-water mark %" PRIu64,
                req_pos, s->hw_pos);
        return PROTO_ERR_BAD_CHECKPOINT;
    }

    /* Acceptance: bump the generation so any orphaned worker aborts. */
    s->generation++;
    s->attempts++;
    s->last_seen = time(NULL);
    if (rem_out) {
        *rem_out = (s->topic->total_bytes > req_pos) ? (s->topic->total_bytes - req_pos) : 0;
    }
    uint32_t gen = s->generation;
    pthread_mutex_unlock(&s->mu);

    log_msg(LOG_INFO, "RESUME accepted: sid=%s, file=%u, offset=%" PRIu64 ", pos=%" PRIu64
            ", remaining=%" PRIu64 ", generation=%u",
            s->sid, file_idx, offset, req_pos, rem_out ? *rem_out : 0, gen);
    return 0;
}

void *session_reaper_thread(void *arg) {
    (void)arg;
    long ttl = session_table_get_ttl();

    while (g_reaper_running) {
        /* Sleep in short slices so shutdown stays responsive. */
        for (int i = 0; i < 5 && g_reaper_running; i++) {
            sleep(1);
        }
        if (!g_reaper_running) break;

        time_t now = time(NULL);
        pthread_mutex_lock(&g_table_mu);
        for (int b = 0; b < BUCKETS; b++) {
            session_t **curr = &g_table[b];
            while (*curr) {
                session_t *entry = *curr;
                bool expired = false;
                bool destroy = false;

                pthread_mutex_lock(&entry->mu);
                if (entry->refs == 0 && (now - entry->last_seen) > ttl) {
                    expired = true;
                }
                pthread_mutex_unlock(&entry->mu);

                if (expired) {
                    *curr = entry->next;
                    entry->next = NULL;
                    if (entry->lrefs > 0) {
                        entry->lrefs--;
                        if (entry->lrefs == 0) destroy = true;
                    }
                    log_msg(LOG_INFO, "Reaper purged expired session: sid=%s", entry->sid);
                    if (destroy) {
                        /* Must not free while holding g_table_mu if another
                         * thread could still be inside session_release(); the
                         * lrefs check above guarantees that is not the case. */
                        session_free(entry);
                    }
                } else {
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
