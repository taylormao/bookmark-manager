# libbookmark_core - 纯 C 核心库（不依赖 GTK，需求 2.2 约束 1）

add_library(bookmark_core STATIC
  bmk_types.c
  bmk_log.c
  bmk_url.c
  bmk_db.c
  bmk_settings.c
  bmk_netscape.c
  bmk_fetch.c
  bmk_validate.c
  bmk_process.c
  bmk_proxy.c
  bmk_backup.c
  bmk_sync.c
  bmk_app.c
)

target_include_directories(bookmark_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(bookmark_core
  PUBLIC
    PkgConfig::SQLITE3
    PkgConfig::CURL
    bmk_cjson
    bmk_gumbo
)

if(WIN32)
  target_link_libraries(bookmark_core PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(bookmark_core PUBLIC Threads::Threads)
  # iconv：glibc 内建，其它平台需显式链接
  find_library(ICONV_LIB iconv)
  if(ICONV_LIB)
    target_link_libraries(bookmark_core PUBLIC ${ICONV_LIB})
  endif()
endif()
