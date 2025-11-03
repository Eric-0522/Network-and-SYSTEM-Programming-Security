// ============================================================
// 這支程式為 client 端，負責與 server 連線並傳送指令。
// 支援三種命令：ping、echo、sysinfo。
// 使用 libutils.so 的共用函式進行封包封裝與傳輸。
// ============================================================
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


static void usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [-h host] [-p port] [-v level] cmd [args...]\n"
    "Commands: ping | sysinfo | echo <text>\n", arg0);
}

int main(int argc, char **argv) {
    log_set_prog("client");
    log_set_level(getenv("LOG_LEVEL")? atoi(getenv("LOG_LEVEL")) : LOG_INFO);
    robust_set_defaults(0);
    const char *host="127.0.0.1", *port="9090";
     // 解析命令列參數
    // 支援 -h, -p, -v, --no-robust
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i], "-h") && i+1<argc) host = argv[++i];
        else if (!strcmp(argv[i], "-p") && i+1<argc) port = argv[++i];
        else if (!strcmp(argv[i], "-v") && i+1<argc) log_set_level(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-robust")) { g_robust.enable_timeouts=0; g_robust.validate_headers=0; g_robust.ignore_sigpipe=0; }
        else { // first non-flag is command
            break;
        }
    }

    if (argc < 2) { usage(argv[0]); return 2; }
    // find command start index
    int cmdi = 1;
    while (cmdi<argc && argv[cmdi][0]=='-') {
        if (!strcmp(argv[cmdi], "-h") || !strcmp(argv[cmdi], "-p") || !strcmp(argv[cmdi], "-v") || !strcmp(argv[cmdi], "--no-robust")) cmdi+= (argv[cmdi][1]=='-'?1:2);
        else break;
    }
    if (cmdi>=argc) { usage(argv[0]); return 2; }
    // 取得命令名稱
    const char *cmd = argv[cmdi];
    // 建立 TCP 連線
    int fd = tcp_connect(host, port, g_robust.io_timeout_ms);
    if (fd<0) { LOGE("connect: %s", strerror(errno)); return 1; }
    set_timeouts(fd, g_robust.io_timeout_ms, g_robust.io_timeout_ms);

    // 根據命令選擇封包類型
    struct msg_hdr h; void *pl=NULL; uint32_t len=0;
    if (!strcmp(cmd, "ping")) {
        send_frame(fd, REQ_PING, "ping", 4, g_robust.io_timeout_ms);
        // 接收 server 回應
        if (recv_frame(fd, &h, &pl, &len, g_robust.io_timeout_ms)==0 && ntohs(h.type)==RESP_PING) {
            fwrite(pl, 1, len, stdout); fputc('\n', stdout);
        } else {
            LOGE("ping failed");
        }
        free(pl);
    } else if (!strcmp(cmd, "sysinfo")) {
        send_frame(fd, REQ_SYSINFO, NULL, 0, g_robust.io_timeout_ms);
        if (recv_frame(fd, &h, &pl, &len, g_robust.io_timeout_ms)==0 && ntohs(h.type)==RESP_SYSINFO) {
            fwrite(pl, 1, len, stdout); fputc('\n', stdout);
        } else {
            LOGE("sysinfo failed");
        }
        free(pl);
    } else if (!strcmp(cmd, "echo")) {
        if (cmdi+1>=argc) { fprintf(stderr, "echo requires text\n"); close(fd); return 2; }
        const char *text = argv[cmdi+1];
        send_frame(fd, REQ_ECHO, text, (uint32_t)strlen(text), g_robust.io_timeout_ms);
        if (recv_frame(fd, &h, &pl, &len, g_robust.io_timeout_ms)==0 && ntohs(h.type)==RESP_ECHO) {
            fwrite(pl, 1, len, stdout); fputc('\n', stdout);
        } else {
            LOGE("echo failed");
        }
        free(pl);
    } else {
        usage(argv[0]);
    }
    close(fd);
    return 0;
}