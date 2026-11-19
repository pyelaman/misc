#pragma once

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace sample {

inline void check_cuda(cudaError_t result, const char* expression,
                       const char* file, int line) {
  if (result == cudaSuccess) {
    return;
  }

  throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                           ": " + expression + " failed: " +
                           cudaGetErrorString(result));
}

#define CUDA_CHECK(expression) \
  ::sample::check_cuda((expression), #expression, __FILE__, __LINE__)

template <typename T>
class DeviceBuffer {
 public:
  explicit DeviceBuffer(std::size_t count) : count_(count) {
    CUDA_CHECK(
        cudaMalloc(reinterpret_cast<void**>(&data_), count_ * sizeof(T)));
  }

  ~DeviceBuffer() {
    if (data_ != nullptr) {
      cudaFree(data_);
    }
  }

  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;

  DeviceBuffer(DeviceBuffer&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)),
        count_(std::exchange(other.count_, 0)) {}

  DeviceBuffer& operator=(DeviceBuffer&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (data_ != nullptr) {
      cudaFree(data_);
    }
    data_ = std::exchange(other.data_, nullptr);
    count_ = std::exchange(other.count_, 0);
    return *this;
  }

  T* get() { return data_; }
  const T* get() const { return data_; }
  std::size_t size() const { return count_; }

 private:
  T* data_ = nullptr;
  std::size_t count_ = 0;
};

class EventTimer {
 public:
  EventTimer() {
    CUDA_CHECK(cudaEventCreate(&start_));
    CUDA_CHECK(cudaEventCreate(&stop_));
  }

  ~EventTimer() {
    cudaEventDestroy(start_);
    cudaEventDestroy(stop_);
  }

  EventTimer(const EventTimer&) = delete;
  EventTimer& operator=(const EventTimer&) = delete;

  template <typename Launch>
  float average_milliseconds(int repetitions, Launch&& launch) {
    CUDA_CHECK(cudaEventRecord(start_));
    for (int i = 0; i < repetitions; ++i) {
      launch();
    }
    CUDA_CHECK(cudaEventRecord(stop_));
    CUDA_CHECK(cudaEventSynchronize(stop_));

    float elapsed_ms = 0.0F;
    CUDA_CHECK(cudaEventElapsedTime(&elapsed_ms, start_, stop_));
    return elapsed_ms / static_cast<float>(repetitions);
  }

 private:
  cudaEvent_t start_{};
  cudaEvent_t stop_{};
};

__host__ __device__ inline std::uint32_t deterministic_work(
    std::uint32_t seed, int iterations) {
  for (int i = 0; i < iterations; ++i) {
    seed = seed * 1664525U + 1013904223U;
    seed ^= seed >> 13U;
    seed *= 2246822519U;
  }
  return seed;
}

inline int parse_positive_int(const char* value, const char* option) {
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0 ||
      parsed > static_cast<long>(INT32_MAX)) {
    throw std::invalid_argument(std::string(option) +
                                " requires a positive integer");
  }
  return static_cast<int>(parsed);
}

inline cudaDeviceProp print_device_and_require(int minimum_major) {
  int device = 0;
  CUDA_CHECK(cudaGetDevice(&device));

  cudaDeviceProp properties{};
  CUDA_CHECK(cudaGetDeviceProperties(&properties, device));

  std::cout << "GPU: " << properties.name << " (compute capability "
            << properties.major << "." << properties.minor << ")\n";

  if (properties.major < minimum_major) {
    throw std::runtime_error(
        "This sample requires compute capability " +
        std::to_string(minimum_major) + ".0 or newer");
  }
  return properties;
}

}  // namespace sample
