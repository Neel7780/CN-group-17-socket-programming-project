#define _GNU_SOURCE
#include "metrics.h"

static FILE *g_csv_file = NULL;
static pthread_mutex_t g_metrics_mu = PTHREAD_MUTEX_INITIALIZER;

void metrics_init(const char *csv_path) {
    if (!csv_path || csv_path[0] == '\0') return;
    pthread_mutex_lock(&g_metrics_mu);
    bool write_hdr = (access(csv_path, F_OK) != 0);
    g_csv_file = fopen(csv_path, "a");
    if (g_csv_file && write_hdr) {
        fprintf(g_csv_file, "timestamp_ms,role,event,session_id,conn_id,topic,file_idx,bytes,duration_ms,status\n");
        fflush(g_csv_file);
    }
    pthread_mutex_unlock(&g_metrics_mu);
}

void metrics_log_row(const char *role, const char *event, const char *sid, uint32_t conn_id,
                     const char *topic, uint32_t file_idx, uint64_t bytes,
                     uint64_t duration_ms, const char *status) {
    if (!g_csv_file) return;
    uint64_t t_now = now_ms();
    pthread_mutex_lock(&g_metrics_mu);
    fprintf(g_csv_file, "%lu,%s,%s,%s,%u,%s,%u,%lu,%lu,%s\n",
            t_now, role ? role : "", event ? event : "", sid ? sid : "",
            conn_id, topic ? topic : "", file_idx, bytes, duration_ms, status ? status : "");
    fflush(g_csv_file);
    pthread_mutex_unlock(&g_metrics_mu);
}

void metrics_close(void) {
    pthread_mutex_lock(&g_metrics_mu);
    if (g_csv_file) {
        fclose(g_csv_file);
        g_csv_file = NULL;
    }
    pthread_mutex_unlock(&g_metrics_mu);
}
