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

find_package(PkgConfig REQUIRED)

pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CURL    REQUIRED IMPORTED_TARGET libcurl)

# ---- third_party: cJSON + gumbo ------------------------------------------
add_subdirectory(third_party)

# ---- libbookmark_core -----------------------------------------------------
add_subdirectory(src/core)

# ---- CLI ------------------------------------------------------------------
add_subdirectory(src/cli)

# ---- UI -------------------------------------------------------------------
if(BMK_BUILD_UI)
  pkg_check_modules(GTK4 IMPORTED_TARGET gtk4)
  if(GTK4_FOUND)
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
