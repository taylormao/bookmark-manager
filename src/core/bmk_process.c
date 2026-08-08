/*
 * bmk_process.c - 子进程管理实现（Windows / POSIX）
 */
#include "bmk_process.h"
#include "bmk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <winsock2.h>
#  include <direct.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <netinet/in.h>
#  include <unistd.h>
#endif

struct bmk_proc {
#ifdef _WIN32
    HANDLE handle;
    DWORD  pid;
#else
    pid_t  pid;
#endif
};

/* ------------------------------------------------------------------ */
/* 命令行拼装                                                          */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/* Windows CreateProcess 需要单一命令行串，参数含空格时加引号 */
static char *build_cmdline(const char *const *argv)
{
    bmk_buf b;
    bmk_buf_init(&b);
    for (size_t i = 0; argv[i]; i++) {
        if (i) bmk_buf_append_char(&b, ' ');
        bool need_quote = strchr(argv[i], ' ') != NULL || argv[i][0] == '\0';
        if (need_quote) bmk_buf_append_char(&b, '"');
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '"') bmk_buf_append_char(&b, '\\');
            bmk_buf_append_char(&b, *p);
        }
        if (need_quote) bmk_buf_append_char(&b, '"');
    }
    return bmk_buf_steal(&b);
}
#endif

bmk_err bmk_proc_spawn(const char *const *argv, bmk_proc **out)
{
    if (!argv || !argv[0] || !out) return BMK_ERR_INVALID_ARG;
    *out = NULL;

    bmk_proc *p = (bmk_proc *)calloc(1, sizeof(bmk_proc));
    if (!p) return BMK_ERR_NOMEM;

#ifdef _WIN32
    char *cmd = build_cmdline(argv);
    if (!cmd) { free(p); return BMK_ERR_NOMEM; }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   /* 不弹控制台窗口 */
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) { free(p); return BMK_ERR_SUBPROCESS; }

    CloseHandle(pi.hThread);
    p->handle = pi.hProcess;
    p->pid = pi.dwProcessId;
#else
    pid_t pid = fork();
    if (pid < 0) { free(p); return BMK_ERR_SUBPROCESS; }
    if (pid == 0) {
        /* 子进程：静默输出 */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    p->pid = pid;
#endif

    *out = p;
    return BMK_OK;
}

bool bmk_proc_alive(bmk_proc *p)
{
    if (!p) return false;
#ifdef _WIN32
    if (!p->handle) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(p->handle, &code)) return false;
    return code == STILL_ACTIVE;
#else
    if (p->pid <= 0) return false;
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    return r == 0;
#endif
}

void bmk_proc_terminate(bmk_proc *p)
{
    if (!p) return;
#ifdef _WIN32
    if (p->handle) {
        TerminateProcess(p->handle, 0);
        WaitForSingleObject(p->handle, 2000);
    }
#else
    if (p->pid > 0) {
        kill(p->pid, SIGTERM);
        /* 等待最多 2 秒后强杀 */
        for (int i = 0; i < 20; i++) {
            int status = 0;
            if (waitpid(p->pid, &status, WNOHANG) != 0) return;
            usleep(100 * 1000);
        }
        kill(p->pid, SIGKILL);
        waitpid(p->pid, NULL, 0);
    }
#endif
}

void bmk_proc_free(bmk_proc *p)
{
    if (!p) return;
#ifdef _WIN32
    if (p->handle) CloseHandle(p->handle);
#endif
    free(p);
}

/* ------------------------------------------------------------------ */
/* 同步执行并捕获输出                                                  */
/* ------------------------------------------------------------------ */

bmk_err bmk_proc_run(const char *const *argv, const char *cwd,
                     char **out_text, int *out_code)
{
    if (!argv || !argv[0]) return BMK_ERR_INVALID_ARG;
    if (out_text) *out_text = NULL;
    if (out_code) *out_code = -1;

    /*
     * 用 popen 捕获合并输出。为支持 cwd，先切换目录再恢复
     * （调用集中在同步流程中，不并发）。
     */
    char *saved_cwd = NULL;
    if (cwd && *cwd) {
        char buf[4096];
#ifdef _WIN32
        if (_getcwd(buf, sizeof(buf))) saved_cwd = bmk_strdup(buf);
        if (_chdir(cwd) != 0) { free(saved_cwd); return BMK_ERR_IO; }
#else
        if (getcwd(buf, sizeof(buf))) saved_cwd = bmk_strdup(buf);
        if (chdir(cwd) != 0) { free(saved_cwd); return BMK_ERR_IO; }
#endif
    }

    /* 拼接命令行（含 2>&1） */
    bmk_buf cmd;
    bmk_buf_init(&cmd);
    for (size_t i = 0; argv[i]; i++) {
        if (i) bmk_buf_append_char(&cmd, ' ');
        bool q = strchr(argv[i], ' ') != NULL;
        if (q) bmk_buf_append_char(&cmd, '"');
        bmk_buf_append_str(&cmd, argv[i]);
        if (q) bmk_buf_append_char(&cmd, '"');
    }
    bmk_buf_append_str(&cmd, " 2>&1");

#ifdef _WIN32
    FILE *fp = _popen(cmd.data, "r");
#else
    FILE *fp = popen(cmd.data, "r");
#endif
    bmk_buf_free(&cmd);

    if (!fp) {
        if (saved_cwd) {
#ifdef _WIN32
            _chdir(saved_cwd);
#else
            if (chdir(saved_cwd) != 0) { /* 忽略 */ }
#endif
            free(saved_cwd);
        }
        return BMK_ERR_SUBPROCESS;
    }

    bmk_buf out;
    bmk_buf_init(&out);
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0)
        bmk_buf_append(&out, chunk, got);

#ifdef _WIN32
    int code = _pclose(fp);
#else
    int status = pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    if (saved_cwd) {
#ifdef _WIN32
        _chdir(saved_cwd);
#else
        if (chdir(saved_cwd) != 0) { /* 忽略 */ }
#endif
        free(saved_cwd);
    }

    if (out_code) *out_code = code;
    if (out_text) *out_text = bmk_buf_steal(&out);
    else          bmk_buf_free(&out);

    return (code == 0) ? BMK_OK : BMK_ERR_SUBPROCESS;
}

/* ------------------------------------------------------------------ */

bool bmk_port_is_free(int port)
{
    if (port <= 0 || port > 65535) return false;

#ifdef _WIN32
    WSADATA wsa;
    static int wsa_ready = 0;
    if (!wsa_ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return true;
        wsa_ready = 1;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return true;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return true;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7F000001);  /* 127.0.0.1 */

    int rc = bind(s, (struct sockaddr *)&addr, sizeof(addr));

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return rc == 0;
}

char *bmk_which(const char *exe)
{
    if (!exe || !*exe) return NULL;

    /* 已是绝对/相对路径且存在 */
    if (strchr(exe, '/') || strchr(exe, '\\')) {
        if (bmk_file_exists(exe)) return bmk_strdup(exe);
    }

    const char *path = getenv("PATH");
    if (!path) return NULL;

#ifdef _WIN32
    const char sep = ';';
    const char *exts[] = { ".exe", ".cmd", ".bat", "", NULL };
#else
    const char sep = ':';
    const char *exts[] = { "", NULL };
#endif

    char *copy = bmk_strdup(path);
    if (!copy) return NULL;

    char *result = NULL;
    char *tok = copy;
    while (tok && !result) {
        char *next = strchr(tok, sep);
        if (next) *next = '\0';
        if (*tok) {
            for (int i = 0; exts[i] && !result; i++) {
                char name[512];
                snprintf(name, sizeof(name), "%s%s", exe, exts[i]);
                char *full = bmk_path_join(tok, name);
                if (full && bmk_file_exists(full)) result = full;
                else free(full);
            }
        }
        tok = next ? next + 1 : NULL;
    }
    free(copy);
    return result;
}
