# tests/run_gui_selftest.cmake
#
# CTest 包装脚本：运行 bmk-gui --self-test。
#
# 每次运行前清理专用测试数据目录，保证测试确定性（self-test 的计数
# 断言基于全新数据目录）。应用本体从不删除任何数据，只有本 CTest 包装
# 脚本清理它自己管理的专用目录，绝不触碰用户真实数据。
#
# 入参（由 add_test 通过 -D 传入）：
#   BMK_GUI_EXE        bmk-gui 可执行文件路径
#   SELFTEST_DATA_DIR  专用测试数据目录

if(NOT DEFINED BMK_GUI_EXE OR NOT DEFINED SELFTEST_DATA_DIR)
    message(FATAL_ERROR "run_gui_selftest.cmake: missing -DBMK_GUI_EXE or -DSELFTEST_DATA_DIR")
endif()

file(REMOVE_RECURSE "${SELFTEST_DATA_DIR}")

execute_process(
    COMMAND "${BMK_GUI_EXE}" --self-test --data-dir "${SELFTEST_DATA_DIR}"
    RESULT_VARIABLE _selftest_rc)

if(NOT _selftest_rc EQUAL 0)
    message(FATAL_ERROR "gui-selftest FAILED with exit code ${_selftest_rc}")
endif()
