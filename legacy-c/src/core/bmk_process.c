/*
 * bmk_process.c - 子进程管理实现（Windows / POSIX）
 */
#include "bmk_process.h"
#include "bmk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* winsock2.h 必须早于 windows.h，否则会拉入冲突的 winsock.h 声明 */
#  include <winsock2.h>
#  include <windows.h>
#  include <direct.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <netinet/in.h>
#  include <unistd.h>
#endif

/*
 * 进程级 working directory 是共享状态：bmk_proc_run 为支持 cwd 用
 * 「切过去 → popen → 切回来」实现，并发调用会互相改写对方的工作目录。
 * 当前调用方（同步流程、自测）并不并发，但该约束只靠口头约定太脆弱，
 * 这里用互斥把「切目录 → 恢复」整段串行化；popen 阻塞期间持锁是有意的
 * 取舍——受影响的只是同一进程内的其它子进程调用，而不是网络/UI 线程。
 */
#ifdef _WIN32
static SRWLOCK g_proc_cwd_lock = SRWLOCK_INIT;
#  define PROC_CWD_LOCK()   AcquireSRWLockExclusive(&g_proc_cwd_lock)
#  define PROC_CWD_UNLOCK() ReleaseSRWLockExclusive(&g_proc_cwd_lock)
#else
static pthread_mutex_t g_proc_cwd_lock = PTHREAD_MUTEX_INITIALIZER;
#  define PROC_CWD_LOCK()   pthread_mutex_lock(&g_proc_cwd_lock)
#  define PROC_CWD_UNLOCK() pthread_mutex_unlock(&g_proc_cwd_lock)
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
/*
 * Windows CRT 命令行解析规则（CreateProcess 与 MSVCRT 一致）：
 *   - 参数为空或含空格/制表符时必须用双引号包裹；
 *   - 字面引号前的反斜杠需成对（2n 个反斜杠 + '\"' => n 个反斜杠 + 引号）；
 *   - 闭合引号前的尾随反斜杠同样要成对，否则会转义闭合引号本身。
 * 旧实现只处理了 '"'，路径以反斜杠结尾或含制表符时会被错误切分。
 */
static bool append_crt_arg(bmk_buf *b, const char *arg)
{
    bool need_quote = arg[0] == '\0' || strchr(arg, ' ') != NULL ||
                      strchr(arg, '\t') != NULL;
    if (need_quote && !bmk_buf_append_char(b, '"')) return false;

    size_t backslashes = 0;
    for (const char *p = arg; ; p++) {
        if (*p == '\\') { backslashes++; continue; }

        if (*p == '"') {
            for (size_t i = 0; i < backslashes * 2 + 1; i++)
                if (!bmk_buf_append_char(b, '\\')) return false;
            if (!bmk_buf_append_char(b, '"')) return false;
        } else {
            for (size_t i = 0; i < backslashes; i++)
                if (!bmk_buf_append_char(b, '\\')) return false;
            if (!*p) {
                if (need_quote) {
                    for (size_t i = 0; i < backslashes; i++)
                        if (!bmk_buf_append_char(b, '\\')) return false;
                }
                break;
            }
            if (!bmk_buf_append_char(b, *p)) return false;
        }
        backslashes = 0;
    }
    return !need_quote || bmk_buf_append_char(b, '"');
}

/* Windows CreateProcess 需要单一命令行串 */
static char *build_cmdline(const char *const *argv)
{
    bmk_buf b;
    bmk_buf_init(&b);
    for (size_t i = 0; argv[i]; i++) {
        if (i && !bmk_buf_append_char(&b, ' ')) goto fail;
        if (!append_crt_arg(&b, argv[i])) goto fail;
    }
    return bmk_buf_steal(&b);
fail:
    bmk_buf_free(&b);
    return NULL;
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

/*
 * popen 会经过命令解释器（Windows: cmd.exe /c；POSIX: /bin/sh -c）。
 * argv 中混有用户可控值（git remote / branch / 工作目录），旧实现仅按
 * 「含空格」加引号，不转义任何元字符，可被注入附加命令。
 *
 * cmd.exe 没有可靠的通用转义方案，因此在拼串前直接拒绝危险字符：
 * 宁可返回 BMK_ERR_INVALID_ARG 中止，也不执行被篡改的命令。
 * 保留 '%'：git URL 的百分号编码需要它，且变量展开不会引入新的命令分隔符；
 * 保留括号/冒号等 URL 常用字符。
 */
static bool shell_arg_is_safe(const char *s)
{
    static const char forbidden[] = "&|<>^!`$;\"'";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
        if (strchr(forbidden, (int)*p) != NULL) return false;
    }
    return true;
}

/* 校验通过后引用参数；危险字符已被拒绝，故无需处理内部引号。 */
static bool append_shell_arg(bmk_buf *b, const char *arg)
{
    if (!shell_arg_is_safe(arg)) return false;

    bool quote = arg[0] == '\0' || strchr(arg, ' ') != NULL;
    if (quote && !bmk_buf_append_char(b, '"')) return false;
    if (!bmk_buf_append_str(b, arg)) return false;

    /* 以反斜杠结尾时闭合引号会被转义，需把尾随反斜杠加倍 */
    if (quote) {
        size_t trailing = 0;
        for (size_t i = strlen(arg); i > 0 && arg[i - 1] == '\\'; i--)
            trailing++;
        for (size_t i = 0; i < trailing; i++)
            if (!bmk_buf_append_char(b, '\\')) return false;
    }
    return !quote || bmk_buf_append_char(b, '"');
}

bmk_err bmk_proc_run(const char *const *argv, const char *cwd,
                     char **out_text, int *out_code)
{
    if (!argv || !argv[0]) return BMK_ERR_INVALID_ARG;
    if (out_text) *out_text = NULL;
    if (out_code) *out_code = -1;

    /*
     * 先构建并校验命令串：校验不依赖 cwd，失败时无需切换/恢复目录。
     * 再用 popen 捕获合并输出；为支持 cwd，调用期间切换目录并恢复。
     */
    bmk_buf cmd;
    bmk_buf_init(&cmd);
    bool cmd_ok = true;
    for (size_t i = 0; argv[i] && cmd_ok; i++) {
        if (i) cmd_ok = bmk_buf_append_char(&cmd, ' ');
        if (cmd_ok) cmd_ok = append_shell_arg(&cmd, argv[i]);
    }
    if (cmd_ok) cmd_ok = bmk_buf_append_str(&cmd, " 2>&1");
    if (!cmd_ok) {
        bmk_buf_free(&cmd);
        BMK_WARN("reject subprocess: unsafe shell characters or out of memory");
        return BMK_ERR_INVALID_ARG;
    }

    bool cwd_locked = false;
    char *saved_cwd = NULL;
    if (cwd && *cwd) {
        PROC_CWD_LOCK();
        cwd_locked = true;
        char buf[4096];
#ifdef _WIN32
        if (_getcwd(buf, sizeof(buf))) saved_cwd = bmk_strdup(buf);
        if (_chdir(cwd) != 0) {
            PROC_CWD_UNLOCK();
            cwd_locked = false;
            free(saved_cwd); bmk_buf_free(&cmd); return BMK_ERR_IO;
        }
#else
        if (getcwd(buf, sizeof(buf))) saved_cwd = bmk_strdup(buf);
        if (chdir(cwd) != 0) {
            PROC_CWD_UNLOCK();
            cwd_locked = false;
            free(saved_cwd); bmk_buf_free(&cmd); return BMK_ERR_IO;
        }
#endif
    }

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
        if (cwd_locked) { PROC_CWD_UNLOCK(); cwd_locked = false; }
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
    if (cwd_locked) { PROC_CWD_UNLOCK(); cwd_locked = false; }

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
