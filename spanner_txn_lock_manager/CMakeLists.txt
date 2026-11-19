cmake_minimum_required(VERSION 3.20)
project(spanner_txn_lock_manager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(spanner_lock
    src/types.cc
    src/lock_table.cc
    src/lock_shard.cc
    src/replication.cc
    src/coordinator.cc
)
target_include_directories(spanner_lock PUBLIC include)
target_compile_options(spanner_lock PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(spanner_lock PUBLIC pthread)

add_executable(spanner_lock_demo examples/demo.cc)
target_link_libraries(spanner_lock_demo PRIVATE spanner_lock)
target_compile_options(spanner_lock_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(spanner_lock_tests tests/test_main.cc)
target_link_libraries(spanner_lock_tests PRIVATE spanner_lock)
target_compile_options(spanner_lock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME spanner_lock_tests COMMAND spanner_lock_tests)

