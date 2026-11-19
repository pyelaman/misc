#include <cuda/barrier>
#include <cuda/ptx>
#include <cuda/std/utility>
#include <cuda_runtime.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/cuda_utils.hpp"

namespace ptx = cuda::ptx;

namespace {

constexpr int kThreadsPerBlock = 256;
constexpr int kTileElements = 1024;
constexpr int kTileBytes = kTileElements * static_cast<int>(sizeof(int));
static_assert(kTileBytes % 16 == 0,
              "TMA transfer size must be a multiple of 16 bytes");

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
          << "Usage: asynchronous_transaction_barrier [--blocks N] "
             "[--work N] [--repetitions N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown or incomplete option: " + argument);
    }
  }
  return options;
}

__device__ inline bool is_tma_issuer() {
  // CUDA 12.5 does not yet expose cuda::ptx::elect_sync. Designating thread 0
  // preserves the required single-issuer protocol and keeps this sample
  // buildable with the minimum documented toolkit version.
  return threadIdx.x == 0;
}

__global__ void transaction_barrier_kernel(
    const int* input, int* output, std::uint32_t* independent_checksum,
    int work_iterations) {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 900
  __shared__ alignas(16) int tile[kTileElements];
#pragma nv_diag_suppress static_var_with_dynamic_init
  __shared__ barrier_type barrier;

  if (threadIdx.x == 0) {
    init(&barrier, blockDim.x);
    // Make barrier initialization visible to the TMA async proxy.
    ptx::fence_proxy_async(ptx::space_shared);
  }
  __syncthreads();

  const bool issuer = is_tma_issuer();
  if (issuer) {
    // cp.async.bulk associates completion of this global-to-shared TMA copy
    // with the shared-memory barrier. Transaction expectation is added below.
    ptx::cp_async_bulk(
        ptx::space_cluster, ptx::space_global, tile,
        input + static_cast<std::size_t>(blockIdx.x) * kTileElements,
        kTileBytes, cuda::device::barrier_native_handle(barrier));
  }

  // Every thread contributes one arrival. Only the TMA issuer adds the byte
  // transaction count. The phase cannot complete until both counts reach zero.
  const int expected_transaction_bytes = issuer ? kTileBytes : 0;
  auto token = cuda::device::barrier_arrive_tx(
      barrier, 1, expected_transaction_bytes);

  const int checksum_index =
      static_cast<int>(blockIdx.x) * blockDim.x + threadIdx.x;
  independent_checksum[checksum_index] = sample::deterministic_work(
      static_cast<std::uint32_t>(checksum_index) ^ 0x5A5A5A5AU,
      work_iterations);

  // Wait covers all thread arrivals and completion of the kTileBytes TMA load.
  barrier.wait(cuda::std::move(token));

  for (int i = threadIdx.x; i < kTileElements; i += blockDim.x) {
    output[static_cast<std::size_t>(blockIdx.x) * kTileElements + i] =
        tile[i] + 1;
  }
#endif
}

__global__ void cooperative_copy_baseline_kernel(
    const int* input, int* output, std::uint32_t* independent_checksum,
    int work_iterations) {
  __shared__ int tile[kTileElements];

  const std::size_t tile_offset =
      static_cast<std::size_t>(blockIdx.x) * kTileElements;
  for (int i = threadIdx.x; i < kTileElements; i += blockDim.x) {
    tile[i] = input[tile_offset + i];
  }

  // Unlike the split transaction barrier, no independent work overlaps this
  // cooperative copy and blocking synchronization point.
  __syncthreads();

  const int checksum_index =
      static_cast<int>(blockIdx.x) * blockDim.x + threadIdx.x;
  independent_checksum[checksum_index] = sample::deterministic_work(
      static_cast<std::uint32_t>(checksum_index) ^ 0x5A5A5A5AU,
      work_iterations);

  for (int i = threadIdx.x; i < kTileElements; i += blockDim.x) {
    output[tile_offset + i] = tile[i] + 1;
  }
}

void validate(const Options& options, const std::vector<int>& input,
              const std::vector<int>& output,
              const std::vector<std::uint32_t>& checksums) {
  for (std::size_t i = 0; i < output.size(); ++i) {
    if (output[i] != input[i] + 1) {
      throw std::runtime_error("TMA result mismatch at element " +
                               std::to_string(i));
    }
  }

  for (std::size_t i = 0; i < checksums.size(); ++i) {
    const std::uint32_t expected = sample::deterministic_work(
        static_cast<std::uint32_t>(i) ^ 0x5A5A5A5AU,
        options.independent_work_iterations);
    if (checksums[i] != expected) {
      throw std::runtime_error("Independent-work checksum mismatch at " +
                               std::to_string(i));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    sample::print_device_and_require(9);

    const std::size_t element_count =
        static_cast<std::size_t>(options.blocks) * kTileElements;
    const std::size_t checksum_count =
        static_cast<std::size_t>(options.blocks) * kThreadsPerBlock;

    std::vector<int> input(element_count);
    for (std::size_t i = 0; i < input.size(); ++i) {
      input[i] = static_cast<int>((i * 17U) % 65521U);
    }

    sample::DeviceBuffer<int> device_input(element_count);
    sample::DeviceBuffer<int> device_output(element_count);
    sample::DeviceBuffer<std::uint32_t> device_checksums(checksum_count);
    CUDA_CHECK(cudaMemcpy(device_input.get(), input.data(),
                          input.size() * sizeof(int),
                          cudaMemcpyHostToDevice));

    const dim3 grid(options.blocks);
    const dim3 block(kThreadsPerBlock);

    transaction_barrier_kernel<<<grid, block>>>(
        device_input.get(), device_output.get(), device_checksums.get(),
        options.independent_work_iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    std::vector<int> output(element_count);
    std::vector<std::uint32_t> checksums(checksum_count);
    CUDA_CHECK(cudaMemcpy(output.data(), device_output.get(),
                          output.size() * sizeof(int),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(checksums.data(), device_checksums.get(),
                          checksums.size() * sizeof(std::uint32_t),
                          cudaMemcpyDeviceToHost));
    validate(options, input, output, checksums);

    cooperative_copy_baseline_kernel<<<grid, block>>>(
        device_input.get(), device_output.get(), device_checksums.get(),
        options.independent_work_iterations);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    CUDA_CHECK(cudaMemcpy(output.data(), device_output.get(),
                          output.size() * sizeof(int),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(checksums.data(), device_checksums.get(),
                          checksums.size() * sizeof(std::uint32_t),
                          cudaMemcpyDeviceToHost));
    validate(options, input, output, checksums);

    sample::EventTimer timer;
    const float transaction_ms = timer.average_milliseconds(
        options.repetitions, [&] {
          transaction_barrier_kernel<<<grid, block>>>(
              device_input.get(), device_output.get(), device_checksums.get(),
              options.independent_work_iterations);
        });
    CUDA_CHECK(cudaGetLastError());

    const float baseline_ms = timer.average_milliseconds(
        options.repetitions, [&] {
          cooperative_copy_baseline_kernel<<<grid, block>>>(
              device_input.get(), device_output.get(), device_checksums.get(),
              options.independent_work_iterations);
        });
    CUDA_CHECK(cudaGetLastError());

    std::cout << "Validation: PASS\n"
              << "Transaction count per block: " << kTileBytes << " bytes\n"
              << std::fixed << std::setprecision(4)
              << "TMA transaction barrier: " << transaction_ms
              << " ms/launch\n"
              << "Cooperative-copy baseline: " << baseline_ms
              << " ms/launch\n"
              << "Timing is illustrative; use Nsight Compute for a rigorous "
                 "comparison.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << '\n';
    return 1;
  }
}
