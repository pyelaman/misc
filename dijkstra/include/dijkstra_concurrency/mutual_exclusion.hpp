#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace dijkstra::concurrency {

// The historical algorithms assume sequentially consistent atomic registers.
// Each lock therefore uses std::memory_order_seq_cst deliberately.
class OneBitMutex {
public:
    explicit OneBitMutex(std::size_t process_count);

    OneBitMutex(const OneBitMutex&) = delete;
    OneBitMutex& operator=(const OneBitMutex&) = delete;

    void lock(std::size_t process_id);
    void unlock(std::size_t process_id);
    [[nodiscard]] std::size_t process_count() const noexcept;

private:
    void check_process_id(std::size_t process_id) const;

    std::size_t process_count_;
    std::unique_ptr<std::atomic<bool>[]> flag_;
};

class DekkerMutex {
public:
    explicit DekkerMutex(std::size_t initial_turn = 0);

    DekkerMutex(const DekkerMutex&) = delete;
    DekkerMutex& operator=(const DekkerMutex&) = delete;

    void lock(std::size_t process_id);
    void unlock(std::size_t process_id);

private:
    static void check_process_id(std::size_t process_id);

    std::array<std::atomic<bool>, 2> flag_;
    std::atomic<std::size_t> turn_;
};

class DijkstraMutex {
public:
    explicit DijkstraMutex(
        std::size_t process_count,
        std::size_t initial_turn = 0);

    DijkstraMutex(const DijkstraMutex&) = delete;
    DijkstraMutex& operator=(const DijkstraMutex&) = delete;

    void lock(std::size_t process_id);
    void unlock(std::size_t process_id);
    [[nodiscard]] std::size_t process_count() const noexcept;

private:
    void check_process_id(std::size_t process_id) const;

    std::size_t process_count_;
    std::unique_ptr<std::atomic<bool>[]> flag_;
    std::unique_ptr<std::atomic<bool>[]> idle_;
    std::atomic<std::size_t> turn_;
};

// A blocking counting semaphore matching the P/V abstraction from the chapter.
// Wake-up order is intentionally unspecified, as it is in Dijkstra's definition.
class CountingSemaphore {
public:
    explicit CountingSemaphore(
        std::ptrdiff_t initial_permits,
        std::ptrdiff_t maximum_permits);

    CountingSemaphore(const CountingSemaphore&) = delete;
    CountingSemaphore& operator=(const CountingSemaphore&) = delete;

    void acquire();
    [[nodiscard]] bool try_acquire();
    void release();

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::ptrdiff_t permits_;
    std::ptrdiff_t maximum_permits_;
};

class BinarySemaphore {
public:
    explicit BinarySemaphore(bool initially_available = true);

    BinarySemaphore(const BinarySemaphore&) = delete;
    BinarySemaphore& operator=(const BinarySemaphore&) = delete;

    void acquire();
    [[nodiscard]] bool try_acquire();
    void release();

private:
    CountingSemaphore semaphore_;
};

// BasicLockable adapter for the chapter's P(sem); critical; V(sem) pattern.
class SemaphoreMutex {
public:
    SemaphoreMutex() = default;

    SemaphoreMutex(const SemaphoreMutex&) = delete;
    SemaphoreMutex& operator=(const SemaphoreMutex&) = delete;

    void lock();
    [[nodiscard]] bool try_lock();
    void unlock();

private:
    BinarySemaphore semaphore_;
};

} // namespace dijkstra::concurrency
