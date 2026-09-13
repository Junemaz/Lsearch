cmake_minimum_required(VERSION 3.16)
project(Lsearch VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_compile_options($<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>)

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

# ---- core 核心库（不依赖 UI，Qt GUI 将来直接复用） ----
add_library(lsearch_core STATIC
  core/entry.cpp
  core/util.cpp
  core/config.cpp
  core/db.cpp
  core/search.cpp
  core/indexer.cpp
  core/watcher.cpp
)
target_include_directories(lsearch_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsearch_core PUBLIC SQLite::SQLite3)
target_compile_definitions(lsearch_core PUBLIC _GNU_SOURCE)

# ---- ipc 协议与客户端 ----
add_library(lsearch_ipc STATIC
  ipc/proto.cpp
  ipc/client.cpp
)
target_include_directories(lsearch_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsearch_ipc PUBLIC lsearch_core)

# ---- mcp (stdio MCP 前端，复用 ipc/client) ----
add_library(lsearch_mcp_lib STATIC
  mcp/json.cpp
  mcp/protocol.cpp
)
target_include_directories(lsearch_mcp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsearch_mcp_lib PUBLIC lsearch_ipc lsearch_core)

add_executable(lsearch-mcp mcp/main.cpp)
target_link_libraries(lsearch-mcp PRIVATE lsearch_mcp_lib lsearch_ipc lsearch_core)
install(TARGETS lsearch-mcp RUNTIME DESTINATION bin)

# ---- dbus（会话总线薄桥接，复用 ipc/client + mcp 校验；不触碰 core） ----
# lsearch_dbus_lib 为纯逻辑（可无总线单测）；仅 lsearch-dbus 可执行依赖 libdbus。
add_library(lsearch_dbus_lib STATIC
  dbus/logic.cpp
)
target_include_directories(lsearch_dbus_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lsearch_dbus_lib PUBLIC lsearch_mcp_lib lsearch_ipc lsearch_core)

add_executable(lsearch-dbus dbus/main.cpp)
target_include_directories(lsearch-dbus PRIVATE ${DBUS_INCLUDE_DIRS})
target_link_libraries(lsearch-dbus PRIVATE lsearch_dbus_lib ${DBUS_LIBRARIES})
install(TARGETS lsearch-dbus RUNTIME DESTINATION bin)

# 按需激活：安装 session bus service 文件（Exec 由 CMAKE_INSTALL_FULL_BINDIR 展开，不硬编码）
configure_file(dbus/com.lsearch.Daemon.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/com.lsearch.Daemon.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.lsearch.Daemon.service
        DESTINATION share/dbus-1/services)

# ---- daemon ----
add_executable(lsearchd daemon/main.cpp daemon/daemon.cpp)
target_link_libraries(lsearchd PRIVATE lsearch_core lsearch_ipc)

# ---- cli ----
add_executable(lsearch cli/main.cpp)
target_link_libraries(lsearch PRIVATE lsearch_core lsearch_ipc)

# ---- tui ----
option(BUILD_TUI "Build the ncurses TUI" ON)
if(BUILD_TUI)
  find_library(NCURSESW_LIB NAMES ncursesw)
  find_path(NCURSESW_INC NAMES ncursesw/curses.h)
  if(NCURSESW_LIB AND NCURSESW_INC)
    add_executable(lsearch-tui tui/main.cpp)
    target_include_directories(lsearch-tui PRIVATE ${NCURSESW_INC})
    target_link_libraries(lsearch-tui PRIVATE lsearch_core lsearch_ipc ${NCURSESW_LIB})
    target_compile_definitions(lsearch-tui PRIVATE TUI_USE_NCURSESW _XOPEN_SOURCE_EXTENDED=1)
    install(TARGETS lsearch-tui RUNTIME DESTINATION bin)
  else()
    find_package(Curses REQUIRED)
    add_executable(lsearch-tui tui/main.cpp)
    target_include_directories(lsearch-tui PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(lsearch-tui PRIVATE lsearch_core lsearch_ipc ${CURSES_LIBRARIES})
    install(TARGETS lsearch-tui RUNTIME DESTINATION bin)
  endif()
endif()

# ---- third_party: Yet-Zio/FramelessWindow（无边框窗口库）----
# 已本地化并打补丁：Qt5 兼容（position()→pos()/globalPos()）+ 浅色 CSS
add_library(framelesswindow STATIC
  third_party/FramelessWindow/src/framelesswindow.cpp
  third_party/FramelessWindow/src/FramelessWindow.ui
  third_party/FramelessWindow/src/framelesswindow.qrc
)
target_include_directories(framelesswindow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FramelessWindow/src)
target_link_libraries(framelesswindow PUBLIC Qt5::Widgets)
set_target_properties(framelesswindow PROPERTIES
  AUTOMOC ON AUTOUIC ON AUTORCC ON
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# ---- gui (Qt5) ----
option(BUILD_GUI "Build the Qt5 GUI" ON)
if(BUILD_GUI)
  find_package(Qt5 COMPONENTS Widgets QUIET)
  if(Qt5Widgets_FOUND)
    add_executable(lsearch-gui gui/main.cpp)
    target_include_directories(lsearch-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lsearch-gui PRIVATE lsearch_core lsearch_ipc Qt5::Widgets framelesswindow)
    set_target_properties(lsearch-gui PROPERTIES
      AUTOMOC ON
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON)
    install(TARGETS lsearch-gui RUNTIME DESTINATION bin)
  else()
    message(WARNING "Qt5 Widgets 未找到，跳过 GUI 构建（apt install qtbase5-dev）")
  endif()
endif()

# ---- tests ----
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  enable_testing()
  add_executable(lsearch_tests
    tests/test_main.cpp
    tests/test_config.cpp
    tests/test_search.cpp
    tests/test_db.cpp
    tests/test_mcp.cpp
    tests/test_client.cpp
    tests/test_dbus.cpp
  )
  target_link_libraries(lsearch_tests PRIVATE lsearch_core lsearch_mcp_lib lsearch_dbus_lib)
  add_test(NAME lsearch_tests COMMAND lsearch_tests)
endif()

# ---- install ----
install(TARGETS lsearchd lsearch RUNTIME DESTINATION bin)
install(FILES docs/lsearch.conf.example DESTINATION share/lsearch)
