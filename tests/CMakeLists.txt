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
