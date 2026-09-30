cmake_minimum_required(VERSION 3.20)
project(cpp_log LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

add_library(cpp_log_core
  src/monitor.cpp
  src/storage.cpp
  src/collector.cpp
  src/report.cpp)
target_include_directories(cpp_log_core PUBLIC include)
target_link_libraries(cpp_log_core PUBLIC SQLite::SQLite3)
target_compile_options(cpp_log_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cpp-log main.cpp)
target_link_libraries(cpp-log PRIVATE cpp_log_core)
target_compile_options(cpp-log PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(cpp-log-tests tests/monitor_tests.cpp)
target_link_libraries(cpp-log-tests PRIVATE cpp_log_core)
target_compile_options(cpp-log-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cpp-log-tests COMMAND cpp-log-tests)
