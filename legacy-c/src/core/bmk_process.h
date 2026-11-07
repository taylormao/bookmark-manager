/*
 * bmk_process.h - 子进程管理（需求 2.4）
 *
 * git CLI 与 xray-core 均以子进程方式启动，应用退出时强制结束。
 */
#ifndef BMK_PROCESS_H
#define BMK_PROCESS_H

#include "bmk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmk_proc bmk_proc;

/* 启动子进程（后台运行）。argv 以 NULL 结尾。 */
bmk_err bmk_proc_spawn(const char *const *argv, bmk_proc **out);
bool    bmk_proc_alive(bmk_proc *p);
void    bmk_proc_terminate(bmk_proc *p);
void    bmk_proc_free(bmk_proc *p);

/*
 * 同步执行命令并捕获输出（用于 git CLI）。
 * @param cwd       工作目录，可为 NULL
 * @param out_text  合并后的 stdout+stderr，调用者 free；可为 NULL
 * @param out_code  退出码
 */
bmk_err bmk_proc_run(const char *const *argv, const char *cwd,
                     char **out_text, int *out_code);

/* 探测端口是否空闲（本地 127.0.0.1）。 */
bool bmk_port_is_free(int port);

/* 在 PATH 中查找可执行文件，返回完整路径；未找到返回 NULL。 */
char *bmk_which(const char *exe);

#ifdef __cplusplus
}
#endif
#endif /* BMK_PROCESS_H */
