#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <arpa/inet.h>   
#include <netinet/in.h>

#ifdef __cplusplus
    extern "C" {
#endif

// ===== Protocol =====
#define MSG_MAGIC 0x43534231u /* 'CSB1' */

enum msg_type {
    REQ_PING      = 1,
    RESP_PING     = 2,
    REQ_SYSINFO   = 10,
    RESP_SYSINFO  = 11,
    REQ_ECHO      = 20,
    RESP_ECHO     = 21,
    RESP_ERROR    = 255
};

#pragma pack(push, 1)
struct msg_hdr {
    uint32_t magic;   // MSG_MAGIC, network order
    uint16_t type;    // enum msg_type, network order
    uint16_t flags;   // reserved, network order
    uint32_t length;  // payload bytes, network order
};
#pragma pack(pop)

// ===== Logging (dual-level control) =====
// Compile-time: built only if ENABLE_DEBUG is defined (Makefile)
// Runtime: log level variable set by env LOG_LEVEL or CLI flag

enum log_level { LOG_ERROR=0, LOG_WARN=1, LOG_INFO=2, LOG_DEBUG=3 };

void log_set_level(int lvl);
int  log_get_level(void);
void log_set_prog(const char *name);

void log_msg(int lvl, const char *fmt, ...) __attribute__((format(printf,2,3)));

// Macros: debug compiled out unless ENABLE_DEBUG
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN,  __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO,  __VA_ARGS__)
#ifdef ENABLE_DEBUG
# define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#else
# define LOGD(...) do { } while(0)
#endif

// ===== Sockets/helpers =====
int  tcp_listen(const char *host, const char *port, int backlog);
int  tcp_connect(const char *host, const char *port, int timeout_ms);
int  set_nonblock(int fd, int nb);
int  set_cloexec(int fd);
int  set_timeouts(int fd, int rcv_ms, int snd_ms);

ssize_t readn_timeout(int fd, void *buf, size_t n, int timeout_ms);
ssize_t writen_timeout(int fd, const void *buf, size_t n, int timeout_ms);

// Framed I/O
int  send_frame(int fd, uint16_t type, const void *payload, uint32_t len, int timeout_ms);
int  recv_frame(int fd, struct msg_hdr *hdr_out, void **payload_out, uint32_t *len_out, int timeout_ms);

// Error helpers
int  set_signal_handler(int signum, void (*handler)(int));

// System info
// Returns malloc'd string with a human-readable summary; caller free()
char *get_system_info(void);

// Robustness toggles exposed to both sides
struct robust_opts {
    int enable_timeouts;         // 1=use read/write timeouts
    int io_timeout_ms;           // default 5000
    int validate_headers;        // 1=validate magic/type/len bounds
    int ignore_sigpipe;          // 1=ignore SIGPIPE
    int child_guard_secs;        // per-connection alarm() in server child
};

extern struct robust_opts g_robust;
void robust_set_defaults(int server_side);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_H */
