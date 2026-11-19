#include "dijkstra_concurrency/token_ring.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <stdexcept>

namespace dijkstra::concurrency {
namespace {

std::vector<std::uint32_t> normalize_initial_state(
    const std::size_t process_count,
    const std::uint32_t state_count,
    std::vector<std::uint32_t> initial_state) {
    if (initial_state.empty()) {
        initial_state.assign(process_count, 0);
    }
    if (initial_state.size() != process_count) {
        throw std::invalid_argument("initial token-ring state has the wrong size");
    }
    for (const auto value : initial_state) {
        if (value >= state_count) {
            throw std::invalid_argument("initial token-ring value is outside 0..K-1");
        }
    }
    return initial_state;
}

} // namespace

CoarseTokenRing::CoarseTokenRing(
    const std::size_t process_count,
    const std::uint32_t state_count,
    std::vector<std::uint32_t> initial_state)
    : process_count_(process_count),
      state_count_(state_count),
      state_(std::make_unique<std::atomic<std::uint32_t>[]>(process_count)) {
    if (process_count_ < 2) {
        throw std::invalid_argument("token ring requires at least two processes");
    }
    // The chapter numbers P processes as 0..N, so its K >= N condition is
    // state_count >= process_count - 1.
    if (state_count_ < process_count_ - 1 || state_count_ == 0) {
        throw std::invalid_argument(
            "coarse token ring requires K >= process_count - 1");
    }
    initial_state = normalize_initial_state(
        process_count_, state_count_, std::move(initial_state));
    for (std::size_t i = 0; i < process_count_; ++i) {
        state_[i].store(initial_state[i]);
    }
}

void CoarseTokenRing::check_id(const std::size_t process_id) const {
    if (process_id >= process_count_) {
        throw std::out_of_range("process id is outside token ring");
    }
}

bool CoarseTokenRing::has_token_unlocked(
    const std::size_t process_id) const {
    const std::size_t left =
        (process_id + process_count_ - 1) % process_count_;
    if (process_id == 0) {
        return state_[0].load() == state_[left].load();
    }
    return state_[process_id].load() != state_[left].load();
}

bool CoarseTokenRing::has_token(const std::size_t process_id) const {
    check_id(process_id);
    std::lock_guard lock(action_mutex_);
    return has_token_unlocked(process_id);
}

bool CoarseTokenRing::try_step(const std::size_t process_id) {
    check_id(process_id);
    std::lock_guard lock(action_mutex_);
    if (!has_token_unlocked(process_id)) {
        return false;
    }

    if (process_id == 0) {
        state_[0].store((state_[0].load() + 1) % state_count_);
    } else {
        state_[process_id].store(state_[process_id - 1].load());
    }
    return true;
}

std::size_t CoarseTokenRing::token_count_unlocked() const {
    std::size_t result = 0;
    for (std::size_t i = 0; i < process_count_; ++i) {
        result += has_token_unlocked(i) ? 1U : 0U;
    }
    return result;
}

bool CoarseTokenRing::legitimate() const {
    std::lock_guard lock(action_mutex_);
    return token_count_unlocked() == 1;
}

std::vector<std::uint32_t> CoarseTokenRing::snapshot() const {
    std::lock_guard lock(action_mutex_);
    std::vector<std::uint32_t> result(process_count_);
    for (std::size_t i = 0; i < process_count_; ++i) {
        result[i] = state_[i].load();
    }
    return result;
}

std::size_t CoarseTokenRing::run_until_legitimate(
    const std::size_t max_actions,
    const std::uint32_t scheduler_seed) {
    std::mt19937 generator(scheduler_seed);
    for (std::size_t action = 0; action < max_actions; ++action) {
        if (legitimate()) {
            return action;
        }

        std::vector<std::size_t> enabled;
        enabled.reserve(process_count_);
        {
            std::lock_guard lock(action_mutex_);
            for (std::size_t i = 0; i < process_count_; ++i) {
                if (has_token_unlocked(i)) {
                    enabled.push_back(i);
                }
            }
        }
        if (enabled.empty()) {
            throw std::logic_error("token ring has no enabled process");
        }
        std::uniform_int_distribution<std::size_t> select(
            0, enabled.size() - 1);
        (void)try_step(enabled[select(generator)]);
    }
    return max_actions;
}

FineTokenRing::FineTokenRing(
    const std::size_t process_count,
    const std::uint32_t state_count,
    std::vector<std::uint32_t> initial_state,
    std::vector<ControlState> initial_control)
    : process_count_(process_count),
      state_count_(state_count),
      state_(std::make_unique<std::atomic<std::uint32_t>[]>(process_count)) {
    if (process_count_ < 2) {
        throw std::invalid_argument("token ring requires at least two processes");
    }
    // The finer-grained proof requires K > N for processes 0..N.
    if (state_count_ <= process_count_ - 1) {
        throw std::invalid_argument(
            "fine token ring requires K > process_count - 1");
    }
    initial_state = normalize_initial_state(
        process_count_, state_count_, std::move(initial_state));
    for (std::size_t i = 0; i < process_count_; ++i) {
        state_[i].store(initial_state[i]);
    }

    if (initial_control.empty()) {
        initial_control.assign(
            process_count_, ControlState::await_token);
    }
    if (initial_control.size() != process_count_) {
        throw std::invalid_argument(
            "initial token-ring control state has the wrong size");
    }
    control_ = std::move(initial_control);
}

void FineTokenRing::check_id(const std::size_t process_id) const {
    if (process_id >= process_count_) {
        throw std::out_of_range("process id is outside token ring");
    }
}

bool FineTokenRing::has_token_unlocked(
    const std::size_t process_id) const {
    const std::size_t left =
        (process_id + process_count_ - 1) % process_count_;
    if (process_id == 0) {
        return state_[0].load() == state_[left].load();
    }
    return state_[process_id].load() != state_[left].load();
}

bool FineTokenRing::try_step(const std::size_t process_id) {
    check_id(process_id);
    std::lock_guard lock(action_mutex_);

    if (control_[process_id] == ControlState::await_token) {
        if (!has_token_unlocked(process_id)) {
            return false;
        }
        control_[process_id] = ControlState::update_state;
        return true;
    }

    if (process_id == 0) {
        state_[0].store((state_[0].load() + 1) % state_count_);
    } else {
        // This is intentionally a fresh read. Reading the predecessor once
        // and retaining that old value is the alternate version that the
        // chapter reports is not self-stabilizing for K=N+1=4.
        state_[process_id].store(state_[process_id - 1].load());
    }
    control_[process_id] = ControlState::await_token;
    return true;
}

std::size_t FineTokenRing::token_count_unlocked() const {
    std::size_t result = 0;
    for (std::size_t i = 0; i < process_count_; ++i) {
        result += has_token_unlocked(i) ? 1U : 0U;
    }
    return result;
}

bool FineTokenRing::legitimate_and_settled() const {
    std::lock_guard lock(action_mutex_);
    return token_count_unlocked() == 1 &&
           std::all_of(
               control_.begin(),
               control_.end(),
               [](const ControlState state) {
                   return state == ControlState::await_token;
               });
}

std::vector<std::uint32_t> FineTokenRing::snapshot() const {
    std::lock_guard lock(action_mutex_);
    std::vector<std::uint32_t> result(process_count_);
    for (std::size_t i = 0; i < process_count_; ++i) {
        result[i] = state_[i].load();
    }
    return result;
}

std::vector<FineTokenRing::ControlState>
FineTokenRing::control_snapshot() const {
    std::lock_guard lock(action_mutex_);
    return control_;
}

std::size_t FineTokenRing::run_until_legitimate_and_settled(
    const std::size_t max_actions,
    const std::uint32_t scheduler_seed) {
    std::mt19937 generator(scheduler_seed);
    std::uniform_int_distribution<std::size_t> process(
        0, process_count_ - 1);

    for (std::size_t action = 0; action < max_actions; ++action) {
        if (legitimate_and_settled()) {
            return action;
        }
        // A fair randomized scheduler: retrying disabled processes does not
        // count as an algorithm action.
        std::size_t attempts = 0;
        while (!try_step(process(generator))) {
            if (++attempts > process_count_ * 100) {
                throw std::logic_error(
                    "fine token ring made no schedulable progress");
            }
        }
    }
    return max_actions;
}

} // namespace dijkstra::concurrency
