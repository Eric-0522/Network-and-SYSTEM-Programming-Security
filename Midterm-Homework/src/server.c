// ============================================================
// 這支程式為伺服器端主程式，採用 fork() 模型。
// 每個 client 連線都會由父行程 accept 後 fork 出子行程處理。
// 子行程處理完畢後結束，由 SIGCHLD handler 回收資源。
// ============================================================
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <arpa/inet.h>

// 紀錄目前活躍子行程數量
static volatile sig_atomic_t g_children = 0;
// SIGCHLD handler：回收已結束的子行程
static void sigchld_handler(int sig) {
    (void)sig;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        g_children--;
        LOGI("child %d exited (active=%d)", (int)pid, (int)g_children);
    }
}

// SIGALRM handler：防止子行程卡死
static void sigalrm_handler(int sig) {
    (void)sig; LOGW("child guard timeout, exiting");
    _exit(2);
}

static void usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [-p port] [-l addr] [-v level] [--no-robust] [--max-reqs N]\n", arg0);
}

int main(int argc, char **argv) {
    // 初始化日誌與 Robustness 設定
    log_set_prog("server");
    log_set_level(getenv("LOG_LEVEL")? atoi(getenv("LOG_LEVEL")) : LOG_INFO);
    robust_set_defaults(1);

    const char *port = "9090"; const char *addr = NULL;
    // 解析命令列參數
    // 支援 -p, -l, -v, --no-robust
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i], "-p") && i+1<argc) port = argv[++i];
        else if (!strcmp(argv[i], "-l") && i+1<argc) addr = argv[++i];
        else if (!strcmp(argv[i], "-v") && i+1<argc) log_set_level(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-robust")) { g_robust.enable_timeouts=0; g_robust.validate_headers=0; g_robust.ignore_sigpipe=0; g_robust.child_guard_secs=0; g_robust.max_reqs_per_conn=0; }
        else if (!strcmp(argv[i], "--max-reqs") && i+1<argc) {
            g_robust.max_reqs_per_conn = atoi(argv[++i]);  // 0 = 無上限
        }
        else { usage(argv[0]); return 2; }
    }

    set_signal_handler(SIGCHLD, sigchld_handler); // 設定 SIGCHLD handler
    int lfd = tcp_listen(addr, port, 128);  // 建立監聽 socket
    if (lfd < 0) { LOGE("listen failed: %s", strerror(errno)); return 1; }
    LOGI("listening on %s:%s", addr?addr:"0.0.0.0", port);
    
    // 主迴圈不斷接受新連線
    for (;;) {
        struct sockaddr_storage ss; socklen_t slen = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr*)&ss, &slen); // 接受新連線
        if (cfd < 0) { if (errno==EINTR) continue; LOGE("accept: %s", strerror(errno)); continue; }

        pid_t pid = fork();
        if (pid < 0) { LOGE("fork: %s", strerror(errno)); close(cfd); continue; }
        if (pid == 0) {
            // 子行程：負責處理單一 client
            close(lfd);
            if (g_robust.child_guard_secs>0) { set_signal_handler(SIGALRM, sigalrm_handler); alarm(g_robust.child_guard_secs); }
            set_timeouts(cfd, g_robust.io_timeout_ms, g_robust.io_timeout_ms);
            LOGI("child %d handling client", (int)getpid());
            /* 每連線最大請求數：環境變數 MAX_REQS_PER_CONN 可覆寫，預設 16 */
            int reqs = 0;
            const int max_reqs = g_robust.max_reqs_per_conn; // 0 表示無上限
            LOGD("child %d: max_reqs_per_conn=%d", (int)getpid(), max_reqs);
            // 讀取 client 請求與回應邏輯
            for (;;) {
                struct msg_hdr h; void *pl=NULL; uint32_t len=0;
                if (recv_frame(cfd, &h, &pl, &len, g_robust.io_timeout_ms) < 0) {
                    if (errno == ECONNRESET) {
                        LOGI("client closed connection");      // 正常離線
                    } else {
                        LOGW("client recv error: %s", strerror(errno));
                    }
                    break;
                }
                uint16_t t = ntohs(h.type);
                if (t == REQ_PING) {
                    char pong[64];
                    snprintf(pong, sizeof(pong), "pong from pid %d", (int)getpid()); // 將目前子行程 PID 加入回應
                    send_frame(cfd, RESP_PING, pong, (uint32_t)strlen(pong), g_robust.io_timeout_ms);
                } else if (t == REQ_ECHO) {
                    send_frame(cfd, RESP_ECHO, pl, len, g_robust.io_timeout_ms);
                } else if (t == REQ_SYSINFO) {
                    char *info = get_system_info();
                    if (!info) {
                        const char *err = "sysinfo failed";
                        send_frame(cfd, RESP_ERROR, err, (uint32_t)strlen(err), g_robust.io_timeout_ms);
                    } else {
                        send_frame(cfd, RESP_SYSINFO, info, (uint32_t)strlen(info), g_robust.io_timeout_ms);
                        free(info);
                    }
                } else {
                    const char *err = "unknown request";
                    send_frame(cfd, RESP_ERROR, err, (uint32_t)strlen(err), g_robust.io_timeout_ms);
                }
                free(pl);
                /* 遞增次數並檢查是否達上限 */
                if (max_reqs > 0) {
                    if (++reqs >= max_reqs) {
                        LOGI("child %d: reached max requests per connection (%d), closing",
                            (int)getpid(), max_reqs);
                        break;
                    }
                }
            }
            close(cfd);
            LOGI("child %d done", (int)getpid());
            return 0;
        } else {
            // parent
            g_children++;
            LOGI("forked child pid=%d (active=%d)", (int)pid, (int)g_children);
            close(cfd);
        }
    }
}