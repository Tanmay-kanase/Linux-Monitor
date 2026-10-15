cmake_minimum_required(VERSION 3.14)
project(linux_monitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MONITOR_BUILD_TESTS "Build the unit tests" ON)
option(MONITOR_ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This project uses inotify and /proc, so it only builds on Linux.")
endif()

find_package(Threads REQUIRED)

set(MONITOR_WARNINGS -Wall -Wextra -Wpedantic)
if(MONITOR_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- library: everything except main() so tests can link against it ----------
add_library(monitor_lib STATIC
  src/event.cpp
  src/dir_watcher.cpp
  src/proc_parser.cpp
  src/proc_monitor.cpp
  src/logger.cpp
)
target_include_directories(monitor_lib PUBLIC include)
target_compile_options(monitor_lib PRIVATE ${MONITOR_WARNINGS})
target_link_libraries(monitor_lib PUBLIC Threads::Threads)

# ---- command-line tool ----------------------------------------------------------
add_executable(monitor src/main.cpp)
target_compile_options(monitor PRIVATE ${MONITOR_WARNINGS})
target_link_libraries(monitor PRIVATE monitor_lib)

# ---- tests ----------------------------------------------------------------------
if(MONITOR_BUILD_TESTS)
  enable_testing()

  # Prefer a system GoogleTest (apt install libgtest-dev); otherwise download one.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
  endif()

  # CMake < 3.20 names the imported target GTest::Main instead of GTest::gtest_main.
  if(TARGET GTest::gtest_main)
    set(MONITOR_GTEST_MAIN GTest::gtest_main)
  else()
    set(MONITOR_GTEST_MAIN GTest::Main)
  endif()

  add_executable(monitor_tests
    tests/test_unique_fd.cpp
    tests/test_event_queue.cpp
    tests/test_proc_parser.cpp
    tests/test_dir_watcher.cpp
    tests/test_proc_monitor.cpp
    tests/test_logger.cpp
  )
  target_include_directories(monitor_tests PRIVATE tests)
  target_compile_options(monitor_tests PRIVATE ${MONITOR_WARNINGS})
  target_link_libraries(monitor_tests PRIVATE monitor_lib ${MONITOR_GTEST_MAIN})

  include(GoogleTest)
  gtest_discover_tests(monitor_tests)
endif()
