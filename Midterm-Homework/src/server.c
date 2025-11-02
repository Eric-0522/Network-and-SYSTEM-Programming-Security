#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <arpa/inet.h>

static volatile sig_atomic_t g_children = 0;
static void sigchld_handler(int sig) {
    (void)sig;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        g_children--;
        LOGI("child %d exited (active=%d)", (int)pid, (int)g_children);
    }
}

static void sigalrm_handler(int sig) {
    (void)sig; LOGW("child guard timeout, exiting");
    _exit(2);
}

static void usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [-p port] [-l addr] [-v level] [--no-robust]\n", arg0);
}

int main(int argc, char **argv) {
    log_set_prog("server");
    log_set_level(getenv("LOG_LEVEL")? atoi(getenv("LOG_LEVEL")) : LOG_INFO);
    robust_set_defaults(1);

    const char *port = "9090"; const char *addr = NULL;

    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i], "-p") && i+1<argc) port = argv[++i];
        else if (!strcmp(argv[i], "-l") && i+1<argc) addr = argv[++i];
        else if (!strcmp(argv[i], "-v") && i+1<argc) log_set_level(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-robust")) { g_robust.enable_timeouts=0; g_robust.validate_headers=0; g_robust.ignore_sigpipe=0; g_robust.child_guard_secs=0; }
        else { usage(argv[0]); return 2; }
    }

    set_signal_handler(SIGCHLD, sigchld_handler);
    int lfd = tcp_listen(addr, port, 128);
    if (lfd < 0) { LOGE("listen failed: %s", strerror(errno)); return 1; }
    LOGI("listening on %s:%s", addr?addr:"0.0.0.0", port);

    for (;;) {
        struct sockaddr_storage ss; socklen_t slen = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr*)&ss, &slen);
        if (cfd < 0) { if (errno==EINTR) continue; LOGE("accept: %s", strerror(errno)); continue; }

        pid_t pid = fork();
        if (pid < 0) { LOGE("fork: %s", strerror(errno)); close(cfd); continue; }
        if (pid == 0) {
            // child
            close(lfd);
            if (g_robust.child_guard_secs>0) { set_signal_handler(SIGALRM, sigalrm_handler); alarm(g_robust.child_guard_secs); }
            set_timeouts(cfd, g_robust.io_timeout_ms, g_robust.io_timeout_ms);
            LOGI("child %d handling client", (int)getpid());

            for (;;) {
                struct msg_hdr h; void *pl=NULL; uint32_t len=0;
                if (recv_frame(cfd, &h, &pl, &len, g_robust.io_timeout_ms) < 0) { LOGW("client recv error: %s", strerror(errno)); break; }
                uint16_t t = ntohs(h.type);
                if (t == REQ_PING) {
                    const char *pong = "pong";
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