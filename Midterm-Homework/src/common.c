// ============================================================
// 這支檔案實作 libutils.so 中的共用函式。
// 包含 Logging、Robust設定、Socket 工具、封包傳輸、
// 以及系統資訊擷取等核心邏輯。
// ============================================================
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <stdarg.h>

// ===== Runtime Logging 實作 =====
// 透過 log_set_level / log_msg 控制輸出層級
static int g_log_level = LOG_INFO;
static const char *g_prog = "app";

// 設定 log 層級與名稱
void log_set_level(int lvl) { g_log_level = lvl; }
void log_set_prog(const char *name) { g_prog = name ? name : g_prog; }

// 輸出日誌訊息 (包含時間、PID、層級)
void log_msg(int lvl, const char *fmt, ...) {
    if (lvl > g_log_level) return; // 若訊息層級高於目前設定則不輸出
    const char *tag = (lvl==LOG_ERROR?"ERROR": lvl==LOG_WARN?"WARN": lvl==LOG_INFO?"INFO":"DEBUG");
    struct timeval tv; gettimeofday(&tv, NULL);  // 取得目前時間（含微秒）
    struct tm tm; localtime_r(&tv.tv_sec, &tm);  // 轉成本地時間結構（thread-safe）
    char ts[64]; strftime(ts, sizeof ts, "%F %T", &tm); // 格式化時間字串 YYYY-MM-DD HH:MM:SS
    fprintf(stderr, "%s.%03ld %s[%d] %s: ", ts, tv.tv_usec/1000, g_prog, (int)getpid(), tag); // 印出前綴（時間.毫秒 程式名[PID] 等級: ）
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

// ===== robustness opts =====
struct robust_opts g_robust;
// 設定預設 robustness 參數
void robust_set_defaults(int server_side) {
    g_robust.enable_timeouts = 1;  // 預設啟用 I/O 逾時
    g_robust.io_timeout_ms   = 5000; // I/O 逾時 5 秒
    g_robust.validate_headers= 1; // 啟用封包標頭驗證
    g_robust.ignore_sigpipe  = 1;
    g_robust.child_guard_secs= server_side ? 60 : 0;
    // 預設：server 每個連線最多 16 個請求；
    g_robust.max_reqs_per_conn = server_side ? 16 : 0;
}

// ===== signals =====
static void ignore_pipe(void) {
    /*
    #ifdef SIGPIPE
    #endif
    */
    if (g_robust.ignore_sigpipe)
        signal(SIGPIPE, SIG_IGN);
}

int set_signal_handler(int signum, void (*handler)(int)) {
    struct sigaction sa; 
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler; 
    sigemptyset(&sa.sa_mask);  // 處理期間不額外阻擋其他訊號
    sa.sa_flags = SA_RESTART;  // 被訊號中斷的系統呼叫自動重啟
    return sigaction(signum, &sa, NULL); // 設定訊號動作
}

// ===== Socket 實作 (listen/nonblock_connect/timeouts) =====
// 非阻塞連線：搭配 select 實作連線逾時
static int nonblock_connect(int fd, const struct sockaddr *sa, socklen_t salen, int timeout_ms) {
    int rc = connect(fd, sa, salen); // 嘗試立即連線
    if (rc == 0) return 0;           // 立刻成功則回傳 0
    if (errno != EINPROGRESS) return -1; // 若不是進行中錯誤則失敗

    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds); // 設定寫入等待集合（連線完成會變可寫）
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
    rc = select(fd+1, NULL, &wfds, NULL, &tv); // 等待可寫或逾時
    if (rc <= 0) { errno = (rc==0) ? ETIMEDOUT : errno; return -1; } // 逾時或 select 出錯
    int err=0; socklen_t len=sizeof err; if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)<0) return -1; // 取得 SO_ERROR
    if (err) { errno = err; return -1; }
    return 0;
}
// 設定/清除 O_NONBLOCK
int set_nonblock(int fd, int nb) {
    int fl = fcntl(fd, F_GETFL, 0); // 讀取現有旗標
    if (fl < 0) return -1;          // 失敗則回傳 -1
    if (nb) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK; // 設定或清除非阻塞
    return fcntl(fd, F_SETFL, fl);  // 寫回旗標
}
// 設定 FD_CLOEXEC（exec 時自動關閉）
int set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0); // 取得描述元旗標
    if (fl < 0) return -1;          // 失敗
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC); // 設定 close-on-exec
}
// 設定 socket 接收/送出逾時（以 SO_RCVTIMEO/SO_SNDTIMEO）
int set_timeouts(int fd, int rcv_ms, int snd_ms) {
    struct timeval rcv = { .tv_sec = rcv_ms/1000, .tv_usec = (rcv_ms%1000)*1000 }; // 轉成 struct timeval
    struct timeval snd = { .tv_sec = snd_ms/1000, .tv_usec = (snd_ms%1000)*1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv) < 0) return -1; // 設定接收逾時
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd) < 0) return -1; // 設定送出逾時
    return 0; // 成功
}
// 建立監聽 socket（支援 IPv4/IPv6，host 可為 NULL 代表 ANY）
int tcp_listen(const char *host, const char *port, int backlog) {
    struct addrinfo hints = {0}, *res, *rp; int fd=-1;  // hints 初始化為 0；迭代位址結果
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host, port, &hints, &res); // 解析位址/埠
    if (rc) { LOGE("getaddrinfo: %s", gai_strerror(rc)); return -1; }  // 解析失敗直接返回
    for (rp=res; rp; rp=rp->ai_next) {  // 嘗試每個候選位址
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol); // 建立 socket
        if (fd<0) continue; // 建立失敗換下一個
        int on=1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on); // 允許重用位址
        set_cloexec(fd);   // 設定 close-on-exec
        if (bind(fd, rp->ai_addr, rp->ai_addrlen)==0) { // 綁定成功
            if (listen(fd, backlog)==0) break;          // listen 成功則跳出
        }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);  // 釋放位址資訊
    if (fd<0) return -1;
    ignore_pipe();
    return fd; // 回傳監聽 socket fd
}
// 連線到遠端（含連線逾時、回復阻塞模式）
int tcp_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints = {0}, *res, *rp; int fd=-1;           // 準備解析參數
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;  // IPv4/IPv6、TCP
    int rc = getaddrinfo(host, port, &hints, &res);                // 解析位址/埠
    if (rc) { LOGE("getaddrinfo: %s", gai_strerror(rc)); return -1; }
    for (rp=res; rp; rp=rp->ai_next) {                          // 逐一嘗試候選位址
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);  // 建立 socket
        if (fd<0) continue;          // 建立失敗換下一個     
        set_cloexec(fd);             // 設定 close-on-exec  
        set_nonblock(fd, 1);         // 先設為非阻塞以便自訂逾時
        if (nonblock_connect(fd, rp->ai_addr, rp->ai_addrlen, timeout_ms)==0) { // 連線成功
            set_nonblock(fd, 0);     // 回復阻塞模式
            break;                   
        }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);               // 釋放位址資訊
    if (fd<0) return -1;
    ignore_pipe();
    return fd;  // 回傳連線 socket fd
}
// ===== robust I/O =====
static int wait_io(int fd, int write_mode, int timeout_ms) {    // 依模式等待可讀/可寫
    if (!g_robust.enable_timeouts) return 1; // 若未啟用逾時，直接回傳
    fd_set rfds, wfds; FD_ZERO(&rfds); FD_ZERO(&wfds); // 清空集合
    if (write_mode) FD_SET(fd, &wfds); else FD_SET(fd, &rfds); // 根據模式加入對應集合
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 }; // 準備逾時時間
    int rc = select(fd+1, write_mode?NULL:&rfds, write_mode?&wfds:NULL, NULL, &tv); // 等待事件或逾時
    if (rc <= 0) { errno = (rc==0)? ETIMEDOUT : errno; return 0; }  // 逾時或 select 出錯
    return 1;   // 成功進行 I/O
}
// 讀取固定 n 位元組（含逾時與 EINTR 處理)  
ssize_t readn_timeout(int fd, void *buf, size_t n, int timeout_ms) {
    size_t off=0; char *p=(char*)buf;   // 已讀位移與緩衝區指標
    while (off < n) {                   // 直到讀滿 n 位元組
        if (!wait_io(fd, 0, timeout_ms)) return -1; // 等待可讀（可能逾時）
        ssize_t r = read(fd, p+off, n-off); // 讀取資料
        if (r == 0) { errno = ECONNRESET; return -1; }
        if (r < 0) { if (errno==EINTR) continue; return -1; }
        off += (size_t)r;           // 累積已讀位移
    }
    return (ssize_t)off; // 回傳成功總讀取量
}
// 寫出固定 n 位元組（含逾時與 EINTR 處理）
ssize_t writen_timeout(int fd, const void *buf, size_t n, int timeout_ms) {
    size_t off=0; const char *p=(const char*)buf;
    while (off < n) {       // 直到寫完 n 位元組
        if (!wait_io(fd, 1, timeout_ms)) return -1; // 等待可寫（可能逾時）
        ssize_t r = write(fd, p+off, n-off);    // 寫出資料
        if (r <= 0) { if (r<0 && errno==EINTR) continue; return -1; }
        off += (size_t)r;        // 累積已寫位移
    }
    return (ssize_t)off;    // 回傳成功總寫出量
}
// 驗證 header 
static int validate_hdr(const struct msg_hdr *h) {
    if (!g_robust.validate_headers) return 1;
    if (ntohl(h->magic) != MSG_MAGIC) return 0; // magic 不符
    uint16_t t = ntohs(h->type);
    if (!(t==REQ_PING || t==RESP_PING || t==REQ_SYSINFO || t==RESP_SYSINFO || t==REQ_ECHO || t==RESP_ECHO || t==RESP_ERROR))
        return 0;
    uint32_t len = ntohl(h->length); // 讀取 payload 長度
    if (len > (32*1024*1024)) return 0; // 超過 32MiB 上限視為不合法
    return 1;
}
// 傳送一個 frame（先送 header，再送 payload）
int send_frame(int fd, uint16_t type, const void *payload, uint32_t len, int timeout_ms) {
    struct msg_hdr h = { htonl(MSG_MAGIC), htons(type), htons(0), htonl(len) }; // 準備網路位元序標頭
    if (writen_timeout(fd, &h, sizeof h, timeout_ms) < 0) return -1; // 送出標頭
    if (len && payload) {   // 有 payload 再送出資料
        if (writen_timeout(fd, payload, len, timeout_ms) < 0) return -1;
    }
    return 0;
}
// 接收一個 frame（讀 header → 驗證 → 讀 payload）
int recv_frame(int fd, struct msg_hdr *hdr_out, void **payload_out, uint32_t *len_out, int timeout_ms) {
    struct msg_hdr h;   // 暫存標頭
    if (readn_timeout(fd, &h, sizeof h, timeout_ms) < 0) return -1; // 讀取固定 12 bytes 標頭
    if (!validate_hdr(&h)) { errno = EPROTO; return -1; } // 標頭驗證失敗
    uint32_t len = ntohl(h.length); // 取得負載長度
    void *buf = NULL;   // 預設無 payload
    if (len) { // 若有 payload 則配置記憶體並讀取
        buf = malloc(len);
        if (!buf) return -1;
        if (readn_timeout(fd, buf, len, timeout_ms) < 0) { free(buf); return -1; }
    }
    if (hdr_out) *hdr_out = h;  // 回傳標頭（網路位元序一樣）
    if (payload_out) {
        *payload_out = buf;
    } else {
        free(buf);
    } 
    if (len_out) *len_out = len; // 回傳長度
    return 0;
}

// ===== system info =====
static char *read_file_first(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL; // 讀取檔案第一行
    char *line = NULL; size_t cap = 0; ssize_t n = getline(&line, &cap, f); // 讀一整行（自動配置）
    fclose(f);
    if (n <= 0) { free(line); return NULL; }    // 沒讀到內容則釋放並回傳 NULL
    // trim
    while (n>0 && (line[n-1]=='\n' || line[n-1]=='\r')) { line[--n]='\0'; }
    return line; // caller free
}

char *get_system_info(void) {
    struct utsname uts; uname(&uts); // 取得系統識別（節點名、OS、版本、硬體）
    struct sysinfo si; sysinfo(&si); // 取得系統統計（記憶體、開機時間等)

    char *model = read_file_first("/sys/devices/virtual/dmi/id/product_name"); // 讀取機型

    double up_days = si.uptime / 86400.0; // 換算成天數
    char load[64];
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%63s", load)!=1) strcpy(load, "n/a"); fclose(f);} else strcpy(load, "n/a");

    char buf[1024];
    int n = snprintf(buf, sizeof buf,
        "node=%s sys=%s %s release=%s machine=%s | uptime=%.2fd | mem_total=%luMB free=%luMB | load=%s",
        uts.nodename, uts.sysname, uts.version, uts.release, uts.machine,
        up_days, (unsigned long)(si.totalram/1024/1024), (unsigned long)(si.freeram/1024/1024), load);

    char *out = malloc((size_t)n+1);
    if (!out) { free(model); return NULL; }
    memcpy(out, buf, (size_t)n+1);
    free(model);
    return out;
}