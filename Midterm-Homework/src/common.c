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
    if (lvl > g_log_level) return;
    const char *tag = (lvl==LOG_ERROR?"ERROR": lvl==LOG_WARN?"WARN": lvl==LOG_INFO?"INFO":"DEBUG");
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    char ts[64]; strftime(ts, sizeof ts, "%F %T", &tm);
    fprintf(stderr, "%s.%03ld %s[%d] %s: ", ts, tv.tv_usec/1000, g_prog, (int)getpid(), tag);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

// ===== robustness opts =====
struct robust_opts g_robust;

void robust_set_defaults(int server_side) {
    g_robust.enable_timeouts = 1;
    g_robust.io_timeout_ms   = 5000;
    g_robust.validate_headers= 1;
    g_robust.ignore_sigpipe  = 1;
    g_robust.child_guard_secs= server_side ? 60 : 0;
}

// ===== signals =====
static void ignore_pipe(void) {
    #ifdef SIGPIPE
        if (g_robust.ignore_sigpipe)
            signal(SIGPIPE, SIG_IGN);
    #endif
}

int set_signal_handler(int signum, void (*handler)(int)) {
    struct sigaction sa; 
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler; 
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    return sigaction(signum, &sa, NULL);
}

// ===== Socket 實作 (listen/nonblock_connect/timeouts) =====
static int nonblock_connect(int fd, const struct sockaddr *sa, socklen_t salen, int timeout_ms) {
    int rc = connect(fd, sa, salen);
    if (rc == 0) return 0;
    if (errno != EINPROGRESS) return -1;

    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
    rc = select(fd+1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) { errno = (rc==0) ? ETIMEDOUT : errno; return -1; }
    int err=0; socklen_t len=sizeof err; if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)<0) return -1;
    if (err) { errno = err; return -1; }
    return 0;
}

int set_nonblock(int fd, int nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (nb) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, fl);
}

int set_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

int set_timeouts(int fd, int rcv_ms, int snd_ms) {
    struct timeval rcv = { .tv_sec = rcv_ms/1000, .tv_usec = (rcv_ms%1000)*1000 };
    struct timeval snd = { .tv_sec = snd_ms/1000, .tv_usec = (snd_ms%1000)*1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd) < 0) return -1;
    return 0;
}

int tcp_listen(const char *host, const char *port, int backlog) {
    struct addrinfo hints = {0}, *res, *rp; int fd=-1;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) { LOGE("getaddrinfo: %s", gai_strerror(rc)); return -1; }
    for (rp=res; rp; rp=rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd<0) continue;
        int on=1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        set_cloexec(fd);
        if (bind(fd, rp->ai_addr, rp->ai_addrlen)==0) { if (listen(fd, backlog)==0) break; }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);
    if (fd<0) return -1;
    ignore_pipe();
    return fd;
}

int tcp_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints = {0}, *res, *rp; int fd=-1;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) { LOGE("getaddrinfo: %s", gai_strerror(rc)); return -1; }
    for (rp=res; rp; rp=rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd<0) continue;
        set_cloexec(fd);
        set_nonblock(fd, 1);
        if (nonblock_connect(fd, rp->ai_addr, rp->ai_addrlen, timeout_ms)==0) {
            set_nonblock(fd, 0);
            break;
        }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);
    if (fd<0) return -1;
    ignore_pipe();
    return fd;
}

// ===== robust I/O =====
static int wait_io(int fd, int write_mode, int timeout_ms) {
    if (!g_robust.enable_timeouts) return 1; // proceed without waiting
    fd_set rfds, wfds; FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (write_mode) FD_SET(fd, &wfds); else FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
    int rc = select(fd+1, write_mode?NULL:&rfds, write_mode?&wfds:NULL, NULL, &tv);
    if (rc <= 0) { errno = (rc==0)? ETIMEDOUT : errno; return 0; }
    return 1;
}

ssize_t readn_timeout(int fd, void *buf, size_t n, int timeout_ms) {
    size_t off=0; char *p=(char*)buf;
    while (off < n) {
        if (!wait_io(fd, 0, timeout_ms)) return -1;
        ssize_t r = read(fd, p+off, n-off);
        if (r == 0) { errno = ECONNRESET; return -1; }
        if (r < 0) { if (errno==EINTR) continue; return -1; }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

ssize_t writen_timeout(int fd, const void *buf, size_t n, int timeout_ms) {
    size_t off=0; const char *p=(const char*)buf;
    while (off < n) {
        if (!wait_io(fd, 1, timeout_ms)) return -1;
        ssize_t r = write(fd, p+off, n-off);
        if (r <= 0) { if (r<0 && errno==EINTR) continue; return -1; }
        off += (size_t)r;
    }
    return (ssize_t)off;
}
// 驗證 header 
static int validate_hdr(const struct msg_hdr *h) {
    if (!g_robust.validate_headers) return 1;
    if (ntohl(h->magic) != MSG_MAGIC) return 0;
    uint16_t t = ntohs(h->type);
    if (!(t==REQ_PING || t==RESP_PING || t==REQ_SYSINFO || t==RESP_SYSINFO || t==REQ_ECHO || t==RESP_ECHO || t==RESP_ERROR))
        return 0;
    uint32_t len = ntohl(h->length);
    if (len > (32*1024*1024)) return 0; // 32 MiB hard cap
    return 1;
}

int send_frame(int fd, uint16_t type, const void *payload, uint32_t len, int timeout_ms) {
    struct msg_hdr h = { htonl(MSG_MAGIC), htons(type), htons(0), htonl(len) };
    if (writen_timeout(fd, &h, sizeof h, timeout_ms) < 0) return -1;
    if (len && payload) {
        if (writen_timeout(fd, payload, len, timeout_ms) < 0) return -1;
    }
    return 0;
}

int recv_frame(int fd, struct msg_hdr *hdr_out, void **payload_out, uint32_t *len_out, int timeout_ms) {
    struct msg_hdr h; 
    if (readn_timeout(fd, &h, sizeof h, timeout_ms) < 0) return -1;
    if (!validate_hdr(&h)) { errno = EPROTO; return -1; }
    uint32_t len = ntohl(h.length);
    void *buf = NULL;
    if (len) {
        buf = malloc(len);
        if (!buf) return -1;
        if (readn_timeout(fd, buf, len, timeout_ms) < 0) { free(buf); return -1; }
    }
    if (hdr_out) *hdr_out = h; 
    if (payload_out) {
        *payload_out = buf;
    } else {
        free(buf);
    } 
    if (len_out) *len_out = len;
    return 0;
}

// ===== system info =====
static char *read_file_first(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    char *line = NULL; size_t cap = 0; ssize_t n = getline(&line, &cap, f);
    fclose(f);
    if (n <= 0) { free(line); return NULL; }
    // trim
    while (n>0 && (line[n-1]=='\n' || line[n-1]=='\r')) { line[--n]='\0'; }
    return line; // caller free
}

char *get_system_info(void) {
    struct utsname uts; uname(&uts);
    struct sysinfo si; sysinfo(&si);

    char *model = read_file_first("/sys/devices/virtual/dmi/id/product_name");
    char *cpu   = read_file_first("/proc/cpuinfo");

    double up_days = si.uptime / 86400.0;
    char load[64];
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%63s", load)!=1) strcpy(load, "n/a"); fclose(f);} else strcpy(load, "n/a");

    char buf[1024];
    int n = snprintf(buf, sizeof buf,
        "node=%s sys=%s %s release=%s machine=%s | uptime=%.2fd | mem_total=%luMB free=%luMB | load=%s",
        uts.nodename, uts.sysname, uts.version, uts.release, uts.machine,
        up_days, (unsigned long)(si.totalram/1024/1024), (unsigned long)(si.freeram/1024/1024), load);

    (void)cpu;
    char *out = malloc((size_t)n+1);
    if (!out) { free(model); return NULL; }
    memcpy(out, buf, (size_t)n+1);
    free(model);
    return out;
}