add_executable(bmk-cli main.c)
target_link_libraries(bmk-cli PRIVATE bookmark_core)

# Windows：与测试一致，把运行时 DLL 拷到 bmk-cli 输出目录，保证构建产物可独立运行。
if(WIN32)
  include("${CMAKE_SOURCE_DIR}/cmake/BMKWindowsRuntime.cmake")
  bmk_copy_runtime_dlls(copy_cli_runtime_dlls
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${BMK_CORE_RUNTIME_DLLS})
  add_dependencies(bmk-cli copy_cli_runtime_dlls)
endif()
