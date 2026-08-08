# third_party dependencies.
#
# cJSON and Gumbo are vendored when present under third_party/<name>.
# Otherwise we fall back to pkg-config.
#   - cJSON: REQUIRED（settings 表以 JSON 存储，为硬依赖）
#   - Gumbo: 可选（仅影响标题/图标抓取，缺失时优雅降级）

# ---- cJSON ----------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cJSON/cJSON.c")
  add_library(bmk_cjson STATIC cJSON/cJSON.c)
  target_include_directories(bmk_cjson PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cJSON")
else()
  # 优先静态链接 cJSON：MSYS2/MinGW 下动态 libcjson DLL 与 UCRT 工具链
  # 跨 CRT 分配/释放会导致堆损坏；静态链接把 cJSON 编入目标可执行文件。
  pkg_check_modules(CJSON_PC QUIET libcjson)
  find_path(CJSON_INCLUDE_DIR cJSON.h PATH_SUFFIXES cjson
            HINTS ${CJSON_PC_INCLUDE_DIRS})
  find_library(CJSON_STATIC_LIBRARY NAMES libcjson.a cjson.a
               HINTS ${CJSON_PC_LIBRARY_DIRS})
  if(CJSON_INCLUDE_DIR AND CJSON_STATIC_LIBRARY)
    add_library(bmk_cjson STATIC IMPORTED GLOBAL)
    set_target_properties(bmk_cjson PROPERTIES
      IMPORTED_LOCATION "${CJSON_STATIC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CJSON_INCLUDE_DIR}")
    message(STATUS "cJSON: static ${CJSON_STATIC_LIBRARY}")
  else()
    pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
    add_library(bmk_cjson INTERFACE)
    target_link_libraries(bmk_cjson INTERFACE PkgConfig::CJSON)
    message(STATUS "cJSON: pkg-config fallback (shared)")
  endif()
endif()

# ---- Gumbo ----------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gumbo/src/gumbo.h")
  file(GLOB GUMBO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/gumbo/src/*.c")
  add_library(bmk_gumbo STATIC ${GUMBO_SRC})
  target_include_directories(bmk_gumbo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/gumbo/src")
  target_compile_definitions(bmk_gumbo PUBLIC BMK_HAVE_GUMBO=1)
else()
  pkg_check_modules(GUMBO IMPORTED_TARGET gumbo)
  if(GUMBO_FOUND)
    add_library(bmk_gumbo INTERFACE)
    target_link_libraries(bmk_gumbo INTERFACE PkgConfig::GUMBO)
    target_compile_definitions(bmk_gumbo INTERFACE BMK_HAVE_GUMBO=1)
  else()
    message(WARNING "Gumbo not found - title/icon extraction disabled")
    add_library(bmk_gumbo INTERFACE)
    target_compile_definitions(bmk_gumbo INTERFACE BMK_HAVE_GUMBO=0)
  endif()
endif()
