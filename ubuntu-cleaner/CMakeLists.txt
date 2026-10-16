cmake_minimum_required(VERSION 3.16)
project(ubuntu_cleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_executable(ubuntu-cleaner
  src/main.cpp
  src/app.cpp
  src/filelist.cpp
  src/probe.cpp
  src/scanner.cpp
  src/trash.cpp
  src/sysinfo.cpp
  src/memclean.cpp
  src/uninstall.cpp
  src/util.cpp
)

target_link_libraries(ubuntu-cleaner PRIVATE PkgConfig::GTK3)
target_compile_options(ubuntu-cleaner PRIVATE -Wall -Wextra)

add_executable(ubuntu-cleaner-tests
  tests/test_core.cpp
  src/filelist.cpp
  src/probe.cpp
  src/util.cpp
)
target_compile_options(ubuntu-cleaner-tests PRIVATE -Wall -Wextra)
