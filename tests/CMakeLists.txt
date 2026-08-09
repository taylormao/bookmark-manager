# 单元测试（需求 7.1）：全部针对 libcore 的纯函数与数据层，不依赖 GTK。

set(BMK_TEST_TARGETS
  test_url
  test_db
  test_netscape
  test_merge
  test_vless
  test_sync
  test_validate
  test_backup
)

foreach(t IN LISTS BMK_TEST_TARGETS)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE bookmark_core)
  target_include_directories(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME ${t} COMMAND ${t})
  # 各用例在自己的工作目录里建临时库文件，避免相互干扰
  set_tests_properties(${t} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 120)
endforeach()

# Windows：测试可执行文件动态链接 mingw64/bin 下的 libcurl-4.dll /
# libsqlite3-0.dll / libgumbo-3.dll 及其传递依赖。运行时 PATH 未必包含该目录，
# 直接跑 ctest 会因 0xc0000135 (STATUS_DLL_NOT_FOUND) 崩溃。这里把全部所需
# DLL 拷贝到测试输出目录（与 exe 同目录，Windows 优先搜索），使 ctest 开箱即过。
if(WIN32)
  include("${CMAKE_SOURCE_DIR}/cmake/BMKWindowsRuntime.cmake")
  bmk_copy_runtime_dlls(copy_test_runtime_dlls
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${BMK_CORE_RUNTIME_DLLS})
  foreach(t IN LISTS BMK_TEST_TARGETS)
    add_dependencies(${t} copy_test_runtime_dlls)
  endforeach()
endif()

# ---- GUI 自动化自测（--self-test）--------------------------------------
# bmk-gui 仅在 GTK4 可用时构建（根 CMakeLists 按 GTK4_FOUND 决定是否 add_subdirectory）。
# 用 --data-dir 隔离到独立临时目录；测试退出码 0=全过，1=有失败，与 CTest 语义一致。
if(TARGET bmk-gui AND WIN32)
  # bmk-gui 除 core DLL 外还需 GTK/Pango/Cairo 等 DLL（位于 mingw64/bin）。
  # ENVIRONMENT 里的 $ENV{PATH} 在 configure 时展开，未必包含该目录；
  # 用 bmk_windows_dll_dir() 推导出 mingw64/bin 并前置到 PATH，避免
  # Windows 加载器弹“缺少 DLL”模态框导致测试挂起。
  bmk_windows_dll_dir(_gui_dll_dir)
  set(_gui_env "PATH=${_gui_dll_dir};$ENV{PATH}")
  set(_gui_data_dir "${CMAKE_CURRENT_BINARY_DIR}/selftest-data")
  add_test(NAME gui-selftest
           COMMAND ${CMAKE_COMMAND}
                   -DBMK_GUI_EXE=$<TARGET_FILE:bmk-gui>
                   -DSELFTEST_DATA_DIR=${_gui_data_dir}
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gui_selftest.cmake")
  set_tests_properties(gui-selftest PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "${_gui_env}")
  # 保证 GUI 自测所需 core DLL 与 exe 同目录（Windows 优先搜索）
  bmk_copy_runtime_dlls(copy_gui_selftest_dlls
    "$<TARGET_FILE_DIR:bmk-gui>"
    ${BMK_CORE_RUNTIME_DLLS})
  add_dependencies(copy_gui_selftest_dlls bmk-gui)
endif()
