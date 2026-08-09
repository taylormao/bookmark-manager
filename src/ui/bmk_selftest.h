/*
 * bmk_selftest.h - bmk-gui --self-test 自动化功能测试
 *
 * 无头/有头环境下对主窗口做功能级自动化验证：通过 GAction 触发
 * 工具栏/菜单对应的动作（等价于点击），逐步骤校验 UI 状态与数据库落库。
 * 不引入额外测试框架，纯 GTK4 + libcore 现有 API。
 */
#ifndef BMK_SELFTEST_H
#define BMK_SELFTEST_H

#include "bmk_ui.h"

G_BEGIN_DECLS

/*
 * 启动 GUI 自动化自测序列。
 * 测试在 GTK idle 回调中逐步骤执行；全部完成后打印 PASS/FAIL 摘要
 * 并退出应用（g_application_quit）。w 必须为已创建的主窗口。
 */
void bmk_selftest_run(BmkWindow *w);

/* 自测是否全部通过（供 main 设置进程退出码）。 */
gboolean bmk_selftest_all_passed(void);

G_END_DECLS
#endif /* BMK_SELFTEST_H */
