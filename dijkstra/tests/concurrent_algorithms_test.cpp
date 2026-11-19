#include "dijkstra_concurrency/dining_philosophers.hpp"
#include "dijkstra_concurrency/mutual_exclusion.hpp"
#include "dijkstra_concurrency/on_the_fly_gc.hpp"
#include "dijkstra_concurrency/termination_detection.hpp"
#include "dijkstra_concurrency/token_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Mutex>
void exercise_process_mutex(
    Mutex& mutex,
    const std::size_t process_count,
    const std::size_t iterations) {
    std::atomic<int> in_critical_section{0};
    std::atomic<bool> overlap{false};
    std::size_t counter = 0;
    std::vector<std::thread> threads;

    for (std::size_t process = 0; process < process_count; ++process) {
        threads.emplace_back([&, process] {
            for (std::size_t iteration = 0;
                 iteration < iterations;
                 ++iteration) {
                mutex.lock(process);
                if (in_critical_section.fetch_add(1) != 0) {
                    overlap.store(true);
                }
                const std::size_t value = counter;
                if ((iteration & 31U) == 0U) {
                    std::this_thread::yield();
                }
                counter = value + 1;
                in_critical_section.fetch_sub(1);
                mutex.unlock(process);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    check(!overlap.load(), "mutual exclusion was violated");
    check(
        counter == process_count * iterations,
        "protected counter lost updates");
}

void test_one_bit_mutex() {
    dijkstra::concurrency::OneBitMutex mutex(4);
    exercise_process_mutex(mutex, 4, 1'000);
}

void test_dekker_mutex() {
    dijkstra::concurrency::DekkerMutex mutex;
    exercise_process_mutex(mutex, 2, 5'000);
}

void test_dijkstra_mutex() {
    dijkstra::concurrency::DijkstraMutex mutex(4);
    exercise_process_mutex(mutex, 4, 1'000);
}

void test_semaphore() {
    using dijkstra::concurrency::CountingSemaphore;
    CountingSemaphore semaphore(2, 2);
    std::atomic<int> occupants{0};
    std::atomic<int> maximum{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            semaphore.acquire();
            const int current = occupants.fetch_add(1) + 1;
            int observed = maximum.load();
            while (observed < current &&
                   !maximum.compare_exchange_weak(observed, current)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            occupants.fetch_sub(1);
            semaphore.release();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    check(maximum.load() <= 2, "counting semaphore exceeded its permits");
    check(maximum.load() == 2, "counting semaphore did not allow concurrency");
}

void test_dining_philosophers() {
    using dijkstra::concurrency::DiningPhilosophers;
    constexpr std::size_t count = 5;
    DiningPhilosophers table(count);
    std::array<bool, count> eating{};
    std::array<std::size_t, count> meals{};
    std::mutex observation_mutex;
    std::atomic<bool> adjacent_overlap{false};
    std::vector<std::thread> philosophers;

    for (std::size_t philosopher = 0; philosopher < count; ++philosopher) {
        philosophers.emplace_back([&, philosopher] {
            for (std::size_t meal = 0; meal < 50; ++meal) {
                table.take_forks(philosopher);
                {
                    std::lock_guard lock(observation_mutex);
                    const std::size_t left =
                        (philosopher + count - 1) % count;
                    const std::size_t right = (philosopher + 1) % count;
                    if (eating[left] || eating[right]) {
                        adjacent_overlap.store(true);
                    }
                    eating[philosopher] = true;
                    ++meals[philosopher];
                }
                std::this_thread::yield();
                {
                    std::lock_guard lock(observation_mutex);
                    eating[philosopher] = false;
                }
                table.put_forks(philosopher);
            }
        });
    }
    for (auto& philosopher : philosophers) {
        philosopher.join();
    }

    check(!adjacent_overlap.load(), "adjacent philosophers ate together");
    check(
        std::all_of(meals.begin(), meals.end(), [](const auto value) {
            return value == 50;
        }),
        "a philosopher did not finish the bounded workload");
}

std::vector<std::uint32_t> random_state(
    const std::size_t count,
    const std::uint32_t values,
    std::mt19937& generator) {
    std::uniform_int_distribution<std::uint32_t> distribution(0, values - 1);
    std::vector<std::uint32_t> result(count);
    std::generate(result.begin(), result.end(), [&] {
        return distribution(generator);
    });
    return result;
}

void test_coarse_token_ring() {
    using dijkstra::concurrency::CoarseTokenRing;
    std::mt19937 generator(42);
    for (std::uint32_t trial = 0; trial < 50; ++trial) {
        CoarseTokenRing ring(5, 5, random_state(5, 5, generator));
        const auto actions =
            ring.run_until_legitimate(100'000, trial + 1);
        check(actions < 100'000, "coarse token ring did not stabilize");
        check(ring.legitimate(), "coarse token ring state is not legitimate");
    }
}

void test_fine_token_ring() {
    using dijkstra::concurrency::FineTokenRing;
    std::mt19937 generator(84);
    std::uniform_int_distribution<int> control(0, 1);
    for (std::uint32_t trial = 0; trial < 50; ++trial) {
        std::vector<FineTokenRing::ControlState> controls(5);
        for (auto& state : controls) {
            state = control(generator) == 0
                ? FineTokenRing::ControlState::await_token
                : FineTokenRing::ControlState::update_state;
        }
        FineTokenRing ring(
            5,
            5,
            random_state(5, 5, generator),
            controls);
        const auto actions =
            ring.run_until_legitimate_and_settled(200'000, trial + 101);
        check(actions < 200'000, "fine token ring did not stabilize");
        check(
            ring.legitimate_and_settled(),
            "fine token ring state is not settled and legitimate");
    }
}

void test_on_the_fly_gc() {
    using Collector =
        dijkstra::concurrency::OnTheFlyGarbageCollector;

    // Nodes 0,1,2 form the reachable graph. Nodes 3 and 4 are garbage.
    Collector collector(
        5,
        2,
        {0},
        {
            1, 2, // root 0
            2, 1, // node 1
            1, 2, // node 2
            3, 3, // garbage
            4, 4, // garbage
        });

    std::atomic<bool> start{false};
    std::thread mutator([&] {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (std::size_t i = 0; i < 20'000; ++i) {
            // Both targets remain reachable through the root's second edge
            // and the 1<->2 cycle, satisfying the algorithm's precondition.
            collector.redirect(0, 0, (i & 1U) == 0U ? 1 : 2);
        }
    });
    std::thread collector_thread([&] {
        start.store(true);
        (void)collector.collect();
        (void)collector.collect();
    });
    mutator.join();
    collector_thread.join();

    check(!collector.is_reclaimed(0), "collector reclaimed a root");
    check(!collector.is_reclaimed(1), "collector reclaimed reachable node 1");
    check(!collector.is_reclaimed(2), "collector reclaimed reachable node 2");
    check(collector.is_reclaimed(3), "collector did not reclaim garbage node 3");
    check(collector.is_reclaimed(4), "collector did not reclaim garbage node 4");
}

void test_tree_termination() {
    using dijkstra::concurrency::TreeTerminationDetector;
    TreeTerminationDetector detector(4);

    const auto to_one = detector.send(0, 1);
    detector.receive(to_one);
    const auto to_two = detector.send(1, 2);
    detector.receive(to_two);
    const auto to_three = detector.send(1, 3);
    detector.receive(to_three);

    detector.go_idle(0);
    detector.go_idle(1);
    check(!detector.detected(), "tree detector reported termination too early");
    detector.go_idle(2);
    check(!detector.detected(), "tree detector ignored active descendant");
    detector.go_idle(3);
    check(detector.detected(), "tree detector did not report termination");
}

void test_ring_termination() {
    using dijkstra::concurrency::RingTerminationDetector;
    RingTerminationDetector detector(4, {true, false, false, false});

    detector.send_message(0, 2);
    detector.go_idle(0);
    detector.go_idle(2);

    check(detector.try_start_probe(), "leader could not start probe");
    check(detector.try_pass_token(3), "process 3 could not pass token");
    check(detector.try_pass_token(2), "process 2 could not pass token");
    check(detector.try_pass_token(1), "process 1 could not pass token");
    // Process 0 sent a message before the probe, and StartProbe whitened it.
    check(detector.detected(), "ring detector did not report termination");

    RingTerminationDetector active_detector(
        4, {true, false, false, false});
    check(active_detector.try_start_probe(), "second probe did not start");
    check(active_detector.try_pass_token(3), "process 3 could not pass token");
    active_detector.send_message(0, 3);
    active_detector.go_idle(0);
    active_detector.go_idle(3);
    check(active_detector.try_pass_token(2), "process 2 could not pass token");
    check(active_detector.try_pass_token(1), "process 1 could not pass token");
    check(
        !active_detector.detected(),
        "black process state did not invalidate a probe");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"one-bit mutex", test_one_bit_mutex},
        {"Dekker mutex", test_dekker_mutex},
        {"Dijkstra mutex", test_dijkstra_mutex},
        {"semaphore", test_semaphore},
        {"dining philosophers", test_dining_philosophers},
        {"coarse token ring", test_coarse_token_ring},
        {"fine token ring", test_fine_token_ring},
        {"on-the-fly garbage collector", test_on_the_fly_gc},
        {"tree termination detector", test_tree_termination},
        {"ring termination detector", test_ring_termination},
    };

    std::size_t failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    if (failed != 0) {
        std::cerr << failed << " test(s) failed\n";
        return 1;
    }
    return 0;
}
