#include <cooperative_groups.h>
#include <cuda/barrier>
#include <cuda/std/utility>
#include <cuda_runtime.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/cuda_utils.hpp"

namespace cg = cooperative_groups;

namespace {

constexpr int kThreadsPerBlock = 256;
using barrier_type = cuda::barrier<cuda::thread_scope_block>;

struct Options {
  int blocks = 64;
  int independent_work_iterations = 512;
  int repetitions = 100;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--blocks" && i + 1 < argc) {
      options.blocks =
          sample::parse_positive_int(argv[++i], "--blocks");
    } else if (argument == "--work" && i + 1 < argc) {
      options.independent_work_iterations =
          sample::parse_positive_int(argv[++i], "--work");
    } else if (argument == "--repetitions" && i + 1 < argc) {
      options.repetitions =
          sample::parse_positive_int(argv[++i], "--repetitions");
    } else if (argument == "--help") {
      std::cout
          << "Usage: asynchronous_barrier [--blocks N] [--work N] "
             "[--repetitions N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown or incomplete option: " + argument);
    }
  }
  return options;
}

__host__ __device__ int producer_iterations(int thread_rank) {
  // Threads in later lanes deliberately take longer to produce their value.
  // Early arrivals can overlap independent work with these late producers.
  return 32 + (thread_rank & 31) * 8;
}

__host__ __device__ int produced_value(int input, int thread_rank) {
  const std::uint32_t code = sample::deterministic_work(
      static_cast<std::uint32_t>(input), producer_iterations(thread_rank));
  return input + static_cast<int>(code & 0x3FFU);
}

__global__ void split_arrive_wait_kernel(
    const int* input, std::uint64_t* block_sum,
    std::uint32_t* independent_checksum, int work_iterations) {
  __shared__ int produced[kThreadsPerBlock];
#pragma nv_diag_suppress static_var_with_dynamic_init
  __shared__ barrier_type barrier;

  const cg::thread_block block = cg::this_thread_block();
  const int thread_rank = static_cast<int>(block.thread_rank());
  const int global_index = static_cast<int>(blockIdx.x) * blockDim.x +
                           thread_rank;

  if (thread_rank == 0) {
    init(&barrier, block.size());
  }
  block.sync();  // Bootstrap: the barrier must be initialized before arrival.

  produced[thread_rank] =
      produced_value(input[global_index], thread_rank);

  // Arrive is non-blocking. The token identifies this barrier phase.
  barrier_type::arrival_token token = barrier.arrive();

  // This work is independent of values produced by the other threads.
  independent_checksum[global_index] = sample::deterministic_work(
      static_cast<std::uint32_t>(input[global_index]) ^ 0xA5A5A5A5U,
      work_iterations);

  // Shared values are not consumed until every participant has arrived.
  barrier.wait(cuda::std::move(token));

  std::uint64_t sum = 0;
  for (int i = 0; i < blockDim.x; ++i) {
    sum += static_cast<std::uint64_t>(produced[i]);
  }
  block_sum[global_index] = sum;
}

__global__ void blocking_barrier_kernel(
    const int* input, std::uint64_t* block_sum,
    std::uint32_t* independent_checksum, int work_iterations) {
  __shared__ int produced[kThreadsPerBlock];

  const int thread_rank = static_cast<int>(threadIdx.x);
  const int global_index = static_cast<int>(blockIdx.x) * blockDim.x +
                           thread_rank;

  produced[thread_rank] =
      produced_value(input[global_index], thread_rank);

  // A conventional barrier blocks early producers here.
  __syncthreads();

  independent_checksum[global_index] = sample::deterministic_work(
      static_cast<std::uint32_t>(input[global_index]) ^ 0xA5A5A5A5U,
      work_iterations);

  std::uint64_t sum = 0;
  for (int i = 0; i < blockDim.x; ++i) {
    sum += static_cast<std::uint64_t>(produced[i]);
  }
  block_sum[global_index] = sum;
}

void validate(const Options& options, const std::vector<int>& input,
              const std::vector<std::uint64_t>& sums,
              const std::vector<std::uint32_t>& checksums) {
  for (int block = 0; block < options.blocks; ++block) {
    std::uint64_t expected_sum = 0;
    for (int thread = 0; thread < kThreadsPerBlock; ++thread) {
      const int index = block * kThreadsPerBlock + thread;
      expected_sum +=
          static_cast<std::uint64_t>(produced_value(input[index], thread));

      const std::uint32_t expected_checksum = sample::deterministic_work(
          static_cast<std::uint32_t>(input[index]) ^ 0xA5A5A5A5U,
          options.independent_work_iterations);
      if (checksums[index] != expected_checksum) {
        throw std::runtime_error("Independent-work checksum mismatch at " +
                                 std::to_string(index));
      }
      if (sums[index] != expected_sum && thread == kThreadsPerBlock - 1) {
        throw std::runtime_error("Block sum mismatch in block " +
                                 std::to_string(block));
      }
    }

    for (int thread = 0; thread < kThreadsPerBlock; ++thread) {
      const int index = block * kThreadsPerBlock + thread;
      if (sums[index] != expected_sum) {
        throw std::runtime_error("Barrier visibility failure at " +
                                 std::to_string(index));
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    sample::print_device_and_require(8);

    const std::size_t element_count =
        static_cast<std::size_t>(options.blocks) * kThreadsPerBlock;
    std::vector<int> input(element_count);
    for (std::size_t i = 0; i < input.size(); ++i) {
      input[i] = static_cast<int>((i % 4093U) + 1U);
    }

    sample::DeviceBuffer<int> device_input(element_count);
    sample::DeviceBuffer<std::uint64_t> device_sums(element_count);
    sample::DeviceBuffer<std::uint32_t> device_checksums(element_count);
    CUDA_CHECK(cudaMemcpy(device_input.get(), input.data(),
                          input.size() * sizeof(int),
                          cudaMemcpyHostToDevice));

    const dim3 grid(options.blocks);
    const dim3 block(kThreadsPerBlock);

    split_arrive_wait_kernel<<<grid, block>>>(
        device_input.get(), device_sums.get(), device_checksums.get(),
        options.independent_work_iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    std::vector<std::uint64_t> sums(element_count);
    std::vector<std::uint32_t> checksums(element_count);
    CUDA_CHECK(cudaMemcpy(sums.data(), device_sums.get(),
                          sums.size() * sizeof(std::uint64_t),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(checksums.data(), device_checksums.get(),
                          checksums.size() * sizeof(std::uint32_t),
                          cudaMemcpyDeviceToHost));
    validate(options, input, sums, checksums);

    blocking_barrier_kernel<<<grid, block>>>(
        device_input.get(), device_sums.get(), device_checksums.get(),
        options.independent_work_iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(sums.data(), device_sums.get(),
                          sums.size() * sizeof(std::uint64_t),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(checksums.data(), device_checksums.get(),
                          checksums.size() * sizeof(std::uint32_t),
                          cudaMemcpyDeviceToHost));
    validate(options, input, sums, checksums);

    sample::EventTimer timer;
    const float split_ms = timer.average_milliseconds(
        options.repetitions, [&] {
          split_arrive_wait_kernel<<<grid, block>>>(
              device_input.get(), device_sums.get(), device_checksums.get(),
              options.independent_work_iterations);
        });
    CUDA_CHECK(cudaGetLastError());

    const float blocking_ms = timer.average_milliseconds(
        options.repetitions, [&] {
          blocking_barrier_kernel<<<grid, block>>>(
              device_input.get(), device_sums.get(), device_checksums.get(),
              options.independent_work_iterations);
        });
    CUDA_CHECK(cudaGetLastError());

    std::cout << "Validation: PASS\n"
              << std::fixed << std::setprecision(4)
              << "Split arrive/wait: " << split_ms << " ms/launch\n"
              << "Blocking barrier:  " << blocking_ms << " ms/launch\n"
              << "Timing is illustrative; profile the real workload before "
                 "drawing performance conclusions.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 1;
  }
}
