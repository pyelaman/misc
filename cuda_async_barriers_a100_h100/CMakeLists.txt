cmake_minimum_required(VERSION 3.24)

project(cuda_async_barriers_a100_h100 LANGUAGES CXX CUDA)

find_package(CUDAToolkit 12.5 REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

add_executable(asynchronous_barrier
    asynchronous_barrier/main.cu
)
target_include_directories(asynchronous_barrier PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(asynchronous_barrier PROPERTIES
    CUDA_ARCHITECTURES "80;90"
)

add_executable(asynchronous_transaction_barrier
    asynchronous_transaction_barrier/main.cu
)
target_include_directories(asynchronous_transaction_barrier
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(asynchronous_transaction_barrier PROPERTIES
    CUDA_ARCHITECTURES "90"
)

foreach(target asynchronous_barrier asynchronous_transaction_barrier)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--display-error-number>
    )
endforeach()

