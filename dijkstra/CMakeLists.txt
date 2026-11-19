cmake_minimum_required(VERSION 3.20)

project(
    dijkstra_concurrent_algorithms
    VERSION 1.0.0
    LANGUAGES CXX)

option(DIJKSTRA_BUILD_TESTS "Build the algorithm test executable" ON)
option(DIJKSTRA_BUILD_EXAMPLES "Build the demonstration executable" ON)

find_package(Threads REQUIRED)

add_library(
    dijkstra_concurrency
    src/mutual_exclusion.cpp
    src/dining_philosophers.cpp
    src/token_ring.cpp
    src/on_the_fly_gc.cpp
    src/termination_detection.cpp)

add_library(
    dijkstra::concurrency
    ALIAS dijkstra_concurrency)

target_compile_features(dijkstra_concurrency PUBLIC cxx_std_20)
target_include_directories(
    dijkstra_concurrency
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(dijkstra_concurrency PUBLIC Threads::Threads)
target_compile_options(
    dijkstra_concurrency
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion>)

if(DIJKSTRA_BUILD_EXAMPLES)
    add_executable(dijkstra_demo examples/demo.cpp)
    target_link_libraries(dijkstra_demo PRIVATE dijkstra::concurrency)
endif()

if(DIJKSTRA_BUILD_TESTS)
    include(CTest)
    add_executable(concurrent_algorithms_test tests/concurrent_algorithms_test.cpp)
    target_link_libraries(
        concurrent_algorithms_test
        PRIVATE
            dijkstra::concurrency)
    target_compile_options(
        concurrent_algorithms_test
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
    add_test(NAME concurrent_algorithms COMMAND concurrent_algorithms_test)
endif()
