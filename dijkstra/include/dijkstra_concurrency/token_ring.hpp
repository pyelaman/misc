#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dijkstra::concurrency {

class CoarseTokenRing {
public:
    CoarseTokenRing(
        std::size_t process_count,
        std::uint32_t state_count,
        std::vector<std::uint32_t> initial_state = {});

    CoarseTokenRing(const CoarseTokenRing&) = delete;
    CoarseTokenRing& operator=(const CoarseTokenRing&) = delete;

    [[nodiscard]] bool has_token(std::size_t process_id) const;
    [[nodiscard]] bool try_step(std::size_t process_id);
    [[nodiscard]] bool legitimate() const;
    [[nodiscard]] std::vector<std::uint32_t> snapshot() const;

    // Returns the number of actions used, or max_actions if no legitimate
    // state was observed within the supplied simulation budget.
    std::size_t run_until_legitimate(
        std::size_t max_actions,
        std::uint32_t scheduler_seed = 1);

private:
    void check_id(std::size_t process_id) const;
    [[nodiscard]] bool has_token_unlocked(std::size_t process_id) const;
    [[nodiscard]] std::size_t token_count_unlocked() const;

    std::size_t process_count_;
    std::uint32_t state_count_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> state_;
    mutable std::mutex action_mutex_;
};

class FineTokenRing {
public:
    enum class ControlState {
        await_token,
        update_state,
    };

    FineTokenRing(
        std::size_t process_count,
        std::uint32_t state_count,
        std::vector<std::uint32_t> initial_state = {},
        std::vector<ControlState> initial_control = {});

    FineTokenRing(const FineTokenRing&) = delete;
    FineTokenRing& operator=(const FineTokenRing&) = delete;

    // Executes one atomic control action. An await action and its subsequent
    // update are deliberately separate, matching the finer-grained algorithm.
    [[nodiscard]] bool try_step(std::size_t process_id);
    [[nodiscard]] bool legitimate_and_settled() const;
    [[nodiscard]] std::vector<std::uint32_t> snapshot() const;
    [[nodiscard]] std::vector<ControlState> control_snapshot() const;

    std::size_t run_until_legitimate_and_settled(
        std::size_t max_actions,
        std::uint32_t scheduler_seed = 1);

private:
    void check_id(std::size_t process_id) const;
    [[nodiscard]] bool has_token_unlocked(std::size_t process_id) const;
    [[nodiscard]] std::size_t token_count_unlocked() const;

    std::size_t process_count_;
    std::uint32_t state_count_;
    std::unique_ptr<std::atomic<std::uint32_t>[]> state_;
    std::vector<ControlState> control_;
    mutable std::mutex action_mutex_;
};

} // namespace dijkstra::concurrency
