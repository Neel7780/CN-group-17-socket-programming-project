#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>
#include <dirent.h>

/* MSG_NOSIGNAL is Linux/BSD; macOS uses the SO_NOSIGPIPE socket option. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_LINE 1024
#define DEFAULT_PORT 8080
#define DEFAULT_CONNS 4
#define DEFAULT_BATCH_BYTES (1024 * 1024)       /* ~1 MiB contiguous batch */
#define DEFAULT_CKPT_INTERVAL (64 * 1024)       /* 64 KiB intra-file ckpt */
#define DEFAULT_SESSION_TTL 300                 /* 300 seconds session TTL */
#define DEFAULT_MAX_RETRIES 50
#define DEFAULT_MAX_CLIENTS 128
#define DEFAULT_BACKOFF_MS 100                  /* initial retry backoff */
#define DEFAULT_BACKOFF_MAX_MS 2000             /* backoff ceiling */

/* Exit codes according to Section 7 / Table 8 */
#define EXIT_OK 0
#define EXIT_USAGE 1
#define EXIT_RETRY_LIMIT 2
#define EXIT_TOPIC_NOT_FOUND 3
#define EXIT_IO_ERROR 4

/* Logging levels */
#define LOG_ERR   0
#define LOG_INFO  1
#define LOG_DEBUG 2
#define LOG_WARN  1   /* warnings are shown at the default verbosity */
extern int g_log_level;

void log_msg(int level, const char *fmt, ...);

/* Buffered Line Reader */
typedef struct {
    int fd;
    char buf[8192];
    size_t pos;
    size_t len;
} line_reader_t;

void lr_init(line_reader_t *lr, int fd);
ssize_t lr_readline(line_reader_t *lr, char *out, size_t maxlen);
ssize_t lr_read_exact(line_reader_t *lr, void *out, size_t n);

/* Socket Helpers */
ssize_t send_all(int fd, const void *buf, size_t len);
uint64_t now_ms(void);
uint64_t now_ns(void);

int parse_kv_opt(const char *arg, const char *key, long *val_out);
int ensure_dir_exists(const char *path);
int set_socket_buffers(int fd, int sndbuf, int rcvbuf);
int set_tcp_cork(int fd, int enable);

/* Suppress SIGPIPE on a socket without relying on MSG_NOSIGNAL. */
void sock_disable_sigpipe(int fd);

/*
 * Zero-copy file-to-socket write.
 *
 * On Linux this is the native sendfile(2). On platforms whose sendfile()
 * has a different signature (e.g. macOS) it falls back to a read()/send()
 * loop so that the rest of the code, and the fault budget logic in
 * fault.c, stay identical across platforms.
 *
 * *offset is the file offset to read from and is advanced as bytes are
 * transferred. Returns the number of bytes written, or -1 on error.
 */
ssize_t sock_sendfile_chunk(int out_fd, int in_fd, off_t *offset, size_t count);

#endif /* COMMON_H */
