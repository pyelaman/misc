#include "dijkstra_concurrency/mutual_exclusion.hpp"

#include <stdexcept>
#include <thread>

namespace dijkstra::concurrency {

OneBitMutex::OneBitMutex(const std::size_t process_count)
    : process_count_(process_count),
      flag_(std::make_unique<std::atomic<bool>[]>(process_count)) {
    if (process_count_ < 2) {
        throw std::invalid_argument("OneBitMutex requires at least two processes");
    }
    for (std::size_t i = 0; i < process_count_; ++i) {
        flag_[i].store(true);
    }
}

void OneBitMutex::check_process_id(const std::size_t process_id) const {
    if (process_id >= process_count_) {
        throw std::out_of_range("process id is outside OneBitMutex");
    }
}

void OneBitMutex::lock(const std::size_t process_id) {
    check_process_id(process_id);

    for (;;) {
        flag_[process_id].store(false);
        bool restart = false;

        for (std::size_t other = 0; other < process_id; ++other) {
            if (!flag_[other].load()) {
                flag_[process_id].store(true);
                while (!flag_[other].load()) {
                    std::this_thread::yield();
                }
                restart = true;
                break;
            }
        }

        if (restart) {
            continue;
        }

        for (std::size_t other = process_id + 1;
             other < process_count_;
             ++other) {
            while (!flag_[other].load()) {
                std::this_thread::yield();
            }
        }
        return;
    }
}

void OneBitMutex::unlock(const std::size_t process_id) {
    check_process_id(process_id);
    flag_[process_id].store(true);
}

std::size_t OneBitMutex::process_count() const noexcept {
    return process_count_;
}

DekkerMutex::DekkerMutex(const std::size_t initial_turn)
    : flag_{true, true}, turn_(initial_turn) {
    check_process_id(initial_turn);
}

void DekkerMutex::check_process_id(const std::size_t process_id) {
    if (process_id > 1) {
        throw std::out_of_range("DekkerMutex process id must be 0 or 1");
    }
}

void DekkerMutex::lock(const std::size_t process_id) {
    check_process_id(process_id);
    const std::size_t other = 1 - process_id;

    flag_[process_id].store(false);
    while (!flag_[other].load()) {
        if (turn_.load() == process_id) {
            std::this_thread::yield();
            continue;
        }

        flag_[process_id].store(true);
        while (turn_.load() != process_id) {
            std::this_thread::yield();
        }
        flag_[process_id].store(false);
    }
}

void DekkerMutex::unlock(const std::size_t process_id) {
    check_process_id(process_id);
    turn_.store(1 - process_id);
    flag_[process_id].store(true);
}

DijkstraMutex::DijkstraMutex(
    const std::size_t process_count,
    const std::size_t initial_turn)
    : process_count_(process_count),
      flag_(std::make_unique<std::atomic<bool>[]>(process_count)),
      idle_(std::make_unique<std::atomic<bool>[]>(process_count)),
      turn_(initial_turn) {
    if (process_count_ < 2) {
        throw std::invalid_argument("DijkstraMutex requires at least two processes");
    }
    check_process_id(initial_turn);
    for (std::size_t i = 0; i < process_count_; ++i) {
        flag_[i].store(true);
        idle_[i].store(true);
    }
}

void DijkstraMutex::check_process_id(const std::size_t process_id) const {
    if (process_id >= process_count_) {
        throw std::out_of_range("process id is outside DijkstraMutex");
    }
}

void DijkstraMutex::lock(const std::size_t process_id) {
    check_process_id(process_id);
    idle_[process_id].store(false);

    for (;;) {
        if (turn_.load() != process_id) {
            flag_[process_id].store(true);
            const std::size_t observed_turn = turn_.load();
            if (idle_[observed_turn].load()) {
                turn_.store(process_id);
            }
            std::this_thread::yield();
            continue;
        }

        flag_[process_id].store(false);
        bool conflict = false;
        for (std::size_t other = 0; other < process_count_; ++other) {
            if (other != process_id && !flag_[other].load()) {
                conflict = true;
                break;
            }
        }
        if (!conflict) {
            return;
        }
        std::this_thread::yield();
    }
}

void DijkstraMutex::unlock(const std::size_t process_id) {
    check_process_id(process_id);
    flag_[process_id].store(true);
    idle_[process_id].store(true);
}

std::size_t DijkstraMutex::process_count() const noexcept {
    return process_count_;
}

CountingSemaphore::CountingSemaphore(
    const std::ptrdiff_t initial_permits,
    const std::ptrdiff_t maximum_permits)
    : permits_(initial_permits), maximum_permits_(maximum_permits) {
    if (maximum_permits_ <= 0 ||
        initial_permits < 0 ||
        initial_permits > maximum_permits_) {
        throw std::invalid_argument("invalid semaphore permit range");
    }
}

void CountingSemaphore::acquire() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return permits_ > 0; });
    --permits_;
}

bool CountingSemaphore::try_acquire() {
    std::lock_guard lock(mutex_);
    if (permits_ == 0) {
        return false;
    }
    --permits_;
    return true;
}

void CountingSemaphore::release() {
    {
        std::lock_guard lock(mutex_);
        if (permits_ == maximum_permits_) {
            throw std::logic_error("semaphore release would exceed maximum");
        }
        ++permits_;
    }
    condition_.notify_one();
}

BinarySemaphore::BinarySemaphore(const bool initially_available)
    : semaphore_(initially_available ? 1 : 0, 1) {}

void BinarySemaphore::acquire() {
    semaphore_.acquire();
}

bool BinarySemaphore::try_acquire() {
    return semaphore_.try_acquire();
}

void BinarySemaphore::release() {
    semaphore_.release();
}

void SemaphoreMutex::lock() {
    semaphore_.acquire();
}

bool SemaphoreMutex::try_lock() {
    return semaphore_.try_acquire();
}

void SemaphoreMutex::unlock() {
    semaphore_.release();
}

} // namespace dijkstra::concurrency
