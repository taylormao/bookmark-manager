# cmake/BMKWindowsRuntime.cmake
#
# Windows 运行时 DLL 定位/拷贝辅助（MSYS2/MinGW 工具链）。
#
# 背景：MinGW 下 libcurl/sqlite3/gumbo 以 DLL 动态链接，运行时 PATH 未必
# 包含 mingw64/bin，导致直接运行 exe 或 ctest 报 0xc0000135
# (STATUS_DLL_NOT_FOUND)。本模块把（含传递依赖的）DLL 拷贝到目标可执行
# 文件所在目录，使构建产物自包含，ctest 开箱即过（无需手动设置 PATH）。

# 定位 mingw64/bin 目录：
#   1) 用户可用 cache 变量 BMK_MINGW_BIN 显式指定；
#   2) 否则从 pkg-config 的 libdir（CURL_LIBDIR / SQLITE3_LIBDIR）推导 ../bin；
#   3) 找不到则返回空串（调用方给出警告）。
function(bmk_windows_dll_dir out_var)
    if(DEFINED BMK_MINGW_BIN AND EXISTS "${BMK_MINGW_BIN}")
        set(${out_var} "${BMK_MINGW_BIN}" PARENT_SCOPE)
        return()
    endif()

    set(hints)
    if(CURL_LIBDIR)
        list(APPEND hints "${CURL_LIBDIR}/../bin")
    endif()
    if(SQLITE3_LIBDIR)
        list(APPEND hints "${SQLITE3_LIBDIR}/../bin")
    endif()
    if(CURL_LIBRARY_DIRS)
        foreach(d IN LISTS CURL_LIBRARY_DIRS)
            list(APPEND hints "${d}/../bin")
        endforeach()
    endif()
    foreach(h IN LISTS hints)
        if(EXISTS "${h}" AND IS_DIRECTORY "${h}")
            set(${out_var} "${h}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# 解析一组 DLL 的绝对路径；缺失的给出警告（不致命，构建继续）。
function(bmk_resolve_runtime_dlls out_list)
    bmk_windows_dll_dir(_dll_dir)
    set(resolved)
    set(missing)
    foreach(dll IN LISTS ARGN)
        if(_dll_dir AND EXISTS "${_dll_dir}/${dll}")
            list(APPEND resolved "${_dll_dir}/${dll}")
        else()
            list(APPEND missing "${dll}")
        endif()
    endforeach()
    if(missing)
        message(WARNING "bmk: 运行时 DLL 缺失（可能影响直接运行/ctest）：${missing}")
    endif()
    set(${out_list} "${resolved}" PARENT_SCOPE)
endfunction()

# 创建名为 copy_target 的自定义目标，把 DLL 列表拷贝到 dest_dir（幂等）。
# 依赖方用 add_dependencies(<exe> ${copy_target}) 保证先拷贝后链接。
function(bmk_copy_runtime_dlls copy_target dest_dir)
    bmk_resolve_runtime_dlls(_dlls ${ARGN})
    if(_dlls)
        add_custom_target(${copy_target} ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "${dest_dir}"
            COMMENT "Copying runtime DLLs to ${dest_dir}"
            VERBATIM)
    else()
        # 无可拷 DLL（如 Linux / 非 MinGW 工具链）：仍建空 target，
        # 依赖方无需做平台条件判断。
        add_custom_target(${copy_target} ALL)
    endif()
endfunction()

# libcore 运行时依赖集合（含传递依赖）。
#   - libcurl-4.dll 的传递依赖：brotli / openssl / idn2+iconv+intl+unistring /
#     nghttp2 / nghttp3 / ngtcp2(+ossl) / psl / ssh2 / zstd / zlib；
#   - libsqlite3-0.dll：仅系统 DLL；
#   - libgumbo-3.dll：仅系统 DLL（pkg-config 共享库回退时使用）；
#   - 附带 gcc 运行库（libgcc_s_seh / libwinpthread / libstdc++），
#     与 dist 发布包保持一致，防个别 DLL 间接依赖。
set(BMK_CORE_RUNTIME_DLLS
    libcurl-4.dll
    libsqlite3-0.dll
    libgumbo-3.dll
    libbrotlidec.dll
    libbrotlicommon.dll
    libcrypto-3-x64.dll
    libidn2-0.dll
    libiconv-2.dll
    libintl-8.dll
    libunistring-5.dll
    libnghttp2-14.dll
    libnghttp3-9.dll
    libngtcp2-16.dll
    libngtcp2_crypto_ossl-0.dll
    libpsl-5.dll
    libssh2-1.dll
    libssl-3-x64.dll
    libzstd.dll
    zlib1.dll
    libgcc_s_seh-1.dll
    libwinpthread-1.dll
    libstdc++-6.dll
)
