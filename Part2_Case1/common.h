#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#if defined(__linux__)
#include <sys/sendfile.h>
#endif

/* MSG_NOSIGNAL is Linux-only; elsewhere SIGPIPE is ignored process-wide instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_LINE 1024              /* Max control line length, including '\n' (Section 3.1) */
#define MAX_TOPIC_LEN 63           /* Topic keys are at most 63 characters (Section 2.2) */
#define MAX_NAME_LEN 255           /* File names fit in file_entry_t.name[256] */
#define IO_CHUNK (64 * 1024)       /* Socket/file copy unit, also the fault chunk size */
#define LR_BUF_SIZE (16 * 1024)    /* Line reader buffer */

/* Defaults for the retry logic (Design Document Section 3.3) */
#define DEFAULT_MAX_RETRIES 50     /* Maximum number of GET attempts */
#define DEFAULT_BACKOFF_MS 100     /* Initial retry backoff */
#define DEFAULT_BACKOFF_MAX_MS 2000/* Backoff ceiling */
#define DEFAULT_MAX_CLIENTS 128    /* Concurrent client threads */
#define DEFAULT_SEED 42            /* Seed of the fault random stream */
#define DEFAULT_FAULT_PROB 0.0     /* Per-request failure probability */

/* Exit codes (Section 7, Table 8) */
#define EXIT_OK 0
#define EXIT_USAGE 1
#define EXIT_RETRY_LIMIT 2         /* Transfer did not complete (retries exhausted) */
#define EXIT_TOPIC_NOT_FOUND 3
#define EXIT_IO_ERROR 4

/* Leveled logging to stderr, selected with log=0..3 */
#define LOG_ERR   0
#define LOG_WARN  1
#define LOG_INFO  2
#define LOG_DEBUG 3
extern int g_log_level;

void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/*
 * Buffered line reader (Section 3.1).
 *
 * Control lines and raw file payloads share one TCP byte stream, so a single
 * recv() can return a header together with the first bytes of the payload.
 * lr_readline() keeps whatever follows the '\n' in the buffer, and
 * lr_read_exact() drains that buffer before reading the socket directly, so no
 * payload byte is ever lost or misframed.
 */
typedef struct {
    int fd;
    size_t pos;                    /* Next unread byte in buf */
    size_t len;                    /* Valid bytes in buf */
    uint64_t bytes_in;             /* Total bytes read from fd (wire bytes received) */
    char buf[LR_BUF_SIZE];
} line_reader_t;

void lr_init(line_reader_t *lr, int fd);

/*
 * Reads one '\n'-terminated line into out (NUL-terminated, '\n' and a trailing
 * '\r' stripped). Returns the number of stream bytes consumed (>= 1, so an
 * empty line is distinguishable from EOF), 0 on clean EOF before any byte of a
 * new line, or -1 on error with errno set:
 *   EMSGSIZE  line does not fit in maxlen (protocol violation)
 *   EPROTO    EOF in the middle of a line
 *   other     error from read()
 */
ssize_t lr_readline(line_reader_t *lr, char *out, size_t maxlen);

/*
 * Reads exactly n bytes. Returns n on success, a smaller count if EOF arrives
 * first, or -1 on read error.
 */
ssize_t lr_read_exact(line_reader_t *lr, void *out, size_t n);

/* Socket and file helpers */
ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t write_all(int fd, const void *buf, size_t len);
int ensure_dir_exists(const char *path);

/* Optional socket tuning (Design Document Section 6.2, Table 7) */
int set_socket_buffers(int fd, int sndbuf, int rcvbuf);
int set_tcp_cork(int fd, int enable);
void sock_disable_sigpipe(int fd);

/* Clocks: monotonic for durations, wall clock for cross-process alignment */
uint64_t now_ns(void);
uint64_t now_wall_ns(void);

/* Command-line parsing: positional integers and trailing key=value options */
int parse_port(const char *s, int *port_out);
int parse_kv_opt(const char *arg, const char *key, long *val_out);
int parse_kv_str(const char *arg, const char *key, const char **val_out);

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
