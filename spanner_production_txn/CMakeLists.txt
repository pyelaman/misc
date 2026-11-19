cmake_minimum_required(VERSION 3.20)
project(spanner_production_txn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(spanner_prod
    src/types.cc
    src/codec.cc
    src/mvcc_store.cc
    src/lock_manager.cc
    src/split_state_machine.cc
    src/persistent_replica.cc
    src/split_group.cc
    src/true_time.cc
    src/range_directory.cc
    src/coordinator.cc
)
target_include_directories(spanner_prod PUBLIC include)
target_compile_options(spanner_prod PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(spanner_prod PUBLIC pthread)

add_executable(spanner_prod_demo examples/bank_transfer.cc)
target_link_libraries(spanner_prod_demo PRIVATE spanner_prod)
target_compile_options(spanner_prod_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(spanner_prod_tests tests/test_main.cc)
target_link_libraries(spanner_prod_tests PRIVATE spanner_prod)
target_compile_options(spanner_prod_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME spanner_prod_tests COMMAND spanner_prod_tests)

