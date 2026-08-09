# GTK4 界面（需求 3.2 FR-20 等）。仅在检测到 GTK4 时由根 CMakeLists 引入。

add_executable(bmk-gui WIN32
  main.c
  bmk_i18n.c
  bmk_window.c
  bmk_dialog_edit.c
  bmk_dialog_tasks.c
  bmk_dialog_settings.c
  bmk_selftest.c
)

target_link_libraries(bmk-gui PRIVATE bookmark_core PkgConfig::GTK4)
