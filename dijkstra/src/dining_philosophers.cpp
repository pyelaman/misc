#include "dijkstra_concurrency/dining_philosophers.hpp"

#include <mutex>
#include <stdexcept>

namespace dijkstra::concurrency {

DiningPhilosophers::DiningPhilosophers(
    const std::size_t philosopher_count)
    : philosopher_count_(philosopher_count),
      states_(philosopher_count, State::thinking) {
    if (philosopher_count_ < 2) {
        throw std::invalid_argument(
            "DiningPhilosophers requires at least two philosophers");
    }
    ready_.reserve(philosopher_count_);
    for (std::size_t i = 0; i < philosopher_count_; ++i) {
        ready_.push_back(std::make_unique<BinarySemaphore>(false));
    }
}

void DiningPhilosophers::check_id(const std::size_t philosopher) const {
    if (philosopher >= philosopher_count_) {
        throw std::out_of_range("philosopher id is outside the table");
    }
}

std::size_t DiningPhilosophers::left(
    const std::size_t philosopher) const noexcept {
    return (philosopher + philosopher_count_ - 1) % philosopher_count_;
}

std::size_t DiningPhilosophers::right(
    const std::size_t philosopher) const noexcept {
    return (philosopher + 1) % philosopher_count_;
}

void DiningPhilosophers::test(const std::size_t philosopher) {
    if (states_[philosopher] == State::hungry &&
        states_[left(philosopher)] != State::eating &&
        states_[right(philosopher)] != State::eating) {
        states_[philosopher] = State::eating;
        ready_[philosopher]->release();
    }
}

void DiningPhilosophers::take_forks(const std::size_t philosopher) {
    check_id(philosopher);
    {
        std::lock_guard lock(state_mutex_);
        if (states_[philosopher] != State::thinking) {
            throw std::logic_error("philosopher must be thinking before becoming hungry");
        }
        states_[philosopher] = State::hungry;
        test(philosopher);
    }
    ready_[philosopher]->acquire();
}

void DiningPhilosophers::put_forks(const std::size_t philosopher) {
    check_id(philosopher);
    std::lock_guard lock(state_mutex_);
    if (states_[philosopher] != State::eating) {
        throw std::logic_error("philosopher is not eating");
    }
    states_[philosopher] = State::thinking;
    test(left(philosopher));
    test(right(philosopher));
}

DiningPhilosophers::State DiningPhilosophers::state(
    const std::size_t philosopher) {
    check_id(philosopher);
    std::lock_guard lock(state_mutex_);
    return states_[philosopher];
}

std::size_t DiningPhilosophers::philosopher_count() const noexcept {
    return philosopher_count_;
}

} // namespace dijkstra::concurrency
