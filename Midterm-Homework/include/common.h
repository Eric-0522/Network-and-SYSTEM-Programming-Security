#ifndef COMMON_H
#define COMMON_H
// ============================================================
// 這個標頭檔定義整個專案共用的結構與函式介面。
// 內容包括：封包協定格式、Logging 系統、Socket 工具、
// robust_control 結構 (robust_opts) 與系統資訊擷取函式。
// ============================================================
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <arpa/inet.h>   
#include <netinet/in.h>

#ifdef __cplusplus
    extern "C" {
#endif

// ===== Protocol 定義=====
#define MSG_MAGIC 0x43534231u 

// 封包型別 (client 與 server 通訊指令)
enum msg_type {
    REQ_PING      = 1,
    RESP_PING     = 2,
    REQ_SYSINFO   = 10,
    RESP_SYSINFO  = 11,
    REQ_ECHO      = 20,
    RESP_ECHO     = 21,
    RESP_ERROR    = 255
};

// 封包標頭結構 (固定 12 bytes)
#pragma pack(push, 1)
struct msg_hdr {
    uint32_t magic;   // MSG_MAGIC, network order
    uint16_t type;    // enum msg_type, network order
    uint16_t flags;   // reserved, network order
    uint32_t length;  // payload bytes, network order
};
#pragma pack(pop)

// ===== Logging (雙層除錯控制) =====
// 編譯期：由 ENABLE_DEBUG 控制
// 執行期：透過 -v 參數或 LOG_LEVEL 環境變數控制
enum log_level { LOG_ERROR=0, LOG_WARN=1, LOG_INFO=2, LOG_DEBUG=3 };

void log_set_level(int lvl);
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

// Framed I/O 封包傳輸函式
int  send_frame(int fd, uint16_t type, const void *payload, uint32_t len, int timeout_ms);
int  recv_frame(int fd, struct msg_hdr *hdr_out, void **payload_out, uint32_t *len_out, int timeout_ms);

// Error helpers (信號處理函式)
int  set_signal_handler(int signum, void (*handler)(int));

// System info
// Returns malloc'd string with a human-readable summary; caller free()
char *get_system_info(void);

// Robustness toggles exposed to both sides
struct robust_opts {
    int enable_timeouts;         // 是否啟用 I/O 逾時
    int io_timeout_ms;           // 逾時時間 default 5000
    int validate_headers;        // 是否驗證封包標頭
    int ignore_sigpipe;          // 是否忽略 SIGPIPE
    int child_guard_secs;        // per-connection alarm() in server child
    int max_reqs_per_conn;       // 每連線最大請求數 (0 = unlimited)
};

extern struct robust_opts g_robust;
void robust_set_defaults(int server_side);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_H */
