#pragma once

#include "dijkstra_concurrency/mutual_exclusion.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace dijkstra::concurrency {

class DiningPhilosophers {
public:
    enum class State {
        thinking,
        hungry,
        eating,
    };

    explicit DiningPhilosophers(std::size_t philosopher_count = 5);

    DiningPhilosophers(const DiningPhilosophers&) = delete;
    DiningPhilosophers& operator=(const DiningPhilosophers&) = delete;

    void take_forks(std::size_t philosopher);
    void put_forks(std::size_t philosopher);
    [[nodiscard]] State state(std::size_t philosopher);
    [[nodiscard]] std::size_t philosopher_count() const noexcept;

private:
    void check_id(std::size_t philosopher) const;
    [[nodiscard]] std::size_t left(std::size_t philosopher) const noexcept;
    [[nodiscard]] std::size_t right(std::size_t philosopher) const noexcept;
    void test(std::size_t philosopher);

    std::size_t philosopher_count_;
    SemaphoreMutex state_mutex_;
    std::vector<State> states_;
    std::vector<std::unique_ptr<BinarySemaphore>> ready_;
};

} // namespace dijkstra::concurrency
