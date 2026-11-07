cmake_minimum_required(VERSION 3.20)
project(bookmark-manager C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/utf-8 /W3)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

option(BMK_BUILD_UI "Build the GTK4 user interface" ON)
option(BMK_BUILD_TESTS "Build tests" ON)

# pkg-config 只是优先路径，不是硬依赖：scoop 更新常把 .pc 文件与 current
# junction 一起弄丢，那时应能改用 find_path/find_library 或显式 -D 指定。
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)
  pkg_check_modules(CURL    QUIET IMPORTED_TARGET libcurl)
endif()

# ---- SQLite3 回退 ---------------------------------------------------------
if(NOT TARGET PkgConfig::SQLITE3)
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h
            HINTS ${SQLITE3_ROOT} ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include)
  find_library(SQLITE3_LIBRARY
               NAMES sqlite3 libsqlite3.a libsqlite3.dll.a sqlite3.lib
               HINTS ${SQLITE3_ROOT} ${CMAKE_PREFIX_PATH}
               PATH_SUFFIXES lib)
  if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR
      "SQLite3 development files not found. Install them, or pass "
      "-DSQLITE3_ROOT=<prefix> (with include/ and lib/), or set "
      "-DSQLITE3_INCLUDE_DIR and -DSQLITE3_LIBRARY.")
  endif()
  add_library(PkgConfig::SQLITE3 UNKNOWN IMPORTED)
  set_target_properties(PkgConfig::SQLITE3 PROPERTIES
    IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}")
  get_filename_component(_bmk_sqlite3_libdir "${SQLITE3_LIBRARY}" DIRECTORY)
  set(SQLITE3_LIBDIR "${_bmk_sqlite3_libdir}" CACHE PATH "SQLite3 library dir" FORCE)
  message(STATUS "SQLite3: ${SQLITE3_LIBRARY} (find_library fallback)")
endif()

# ---- libcurl 回退 ---------------------------------------------------------
if(NOT TARGET PkgConfig::CURL)
  find_path(CURL_INCLUDE_DIR curl/curl.h
            HINTS ${CURL_ROOT} ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include)
  find_library(CURL_LIBRARY
               NAMES curl libcurl.a libcurl.dll.a libcurl_imp.lib
               HINTS ${CURL_ROOT} ${CMAKE_PREFIX_PATH}
               PATH_SUFFIXES lib)
  if(NOT CURL_INCLUDE_DIR OR NOT CURL_LIBRARY)
    message(FATAL_ERROR
      "libcurl development files not found. Install them, or pass "
      "-DCURL_ROOT=<prefix> (with include/ and lib/), or set "
      "-DCURL_INCLUDE_DIR and -DCURL_LIBRARY.")
  endif()
  add_library(PkgConfig::CURL UNKNOWN IMPORTED)
  set_target_properties(PkgConfig::CURL PROPERTIES
    IMPORTED_LOCATION "${CURL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")
  get_filename_component(_bmk_curl_libdir "${CURL_LIBRARY}" DIRECTORY)
  set(CURL_LIBDIR "${_bmk_curl_libdir}" CACHE PATH "libcurl library dir" FORCE)
  message(STATUS "libcurl: ${CURL_LIBRARY} (find_library fallback)")
endif()

# ---- third_party: cJSON + gumbo ------------------------------------------
add_subdirectory(third_party)

# ---- libbookmark_core -----------------------------------------------------
add_subdirectory(src/core)

# ---- CLI ------------------------------------------------------------------
add_subdirectory(src/cli)

# ---- UI -------------------------------------------------------------------
if(BMK_BUILD_UI)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
  endif()
  if(NOT TARGET PkgConfig::GTK4)
    # 回退：按 MSYS2 / gvsbuild 的标准目录布局探测（无需 .pc 文件）
    find_path(GTK4_INCLUDE_DIR gtk/gtk.h
              HINTS ${GTK4_ROOT} ${CMAKE_PREFIX_PATH}
              PATH_SUFFIXES include/gtk-4.0)
    find_library(GTK4_LIBRARY NAMES gtk-4 libgtk-4.dll.a gtk-4.lib
                 HINTS ${GTK4_ROOT} ${CMAKE_PREFIX_PATH}
                 PATH_SUFFIXES lib)
    if(GTK4_INCLUDE_DIR AND GTK4_LIBRARY)
      get_filename_component(_bmk_gtk4_prefix "${GTK4_LIBRARY}" DIRECTORY)
      get_filename_component(_bmk_gtk4_prefix "${_bmk_gtk4_prefix}" DIRECTORY)
      set(_bmk_gtk4_incs "${GTK4_INCLUDE_DIR}")
      foreach(_d glib-2.0 gio-win32-2.0 pango-1.0 harfbuzz gdk-pixbuf-2.0
                 cairo graphene-1.0 fribidi pixman-1 freetype2 fontconfig libpng16)
        if(EXISTS "${_bmk_gtk4_prefix}/include/${_d}")
          list(APPEND _bmk_gtk4_incs "${_bmk_gtk4_prefix}/include/${_d}")
        endif()
      endforeach()
      foreach(_d glib-2.0/include graphene-1.0/include)
        if(EXISTS "${_bmk_gtk4_prefix}/lib/${_d}")
          list(APPEND _bmk_gtk4_incs "${_bmk_gtk4_prefix}/lib/${_d}")
        endif()
      endforeach()
      add_library(PkgConfig::GTK4 UNKNOWN IMPORTED)
      set_target_properties(PkgConfig::GTK4 PROPERTIES
        IMPORTED_LOCATION "${GTK4_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_bmk_gtk4_incs}"
        INTERFACE_LINK_LIBRARIES
          "gdk_pixbuf-2.0;gio-2.0;gobject-2.0;glib-2.0;pango-1.0;cairo;harfbuzz;graphene-1.0"
        INTERFACE_LINK_DIRECTORIES "${_bmk_gtk4_prefix}/lib")
      set(GTK4_FOUND TRUE)
      message(STATUS "GTK4: ${GTK4_LIBRARY} (find_library fallback)")
    endif()
  endif()
  if(GTK4_FOUND AND TARGET PkgConfig::GTK4)
    add_subdirectory(src/ui)
  else()
    message(WARNING "GTK4 not found - skipping UI target (core + cli still build)")
  endif()
endif()

# ---- tests ----------------------------------------------------------------
if(BMK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
