#include "dijkstra_concurrency/mutual_exclusion.hpp"
#include "dijkstra_concurrency/termination_detection.hpp"
#include "dijkstra_concurrency/token_ring.hpp"

#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace dijkstra::concurrency;

    DijkstraMutex mutex(4);
    std::size_t protected_counter = 0;
    std::vector<std::thread> workers;
    for (std::size_t process = 0; process < 4; ++process) {
        workers.emplace_back([&, process] {
            for (int iteration = 0; iteration < 1'000; ++iteration) {
                mutex.lock(process);
                ++protected_counter;
                mutex.unlock(process);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    std::cout << "Dijkstra mutex protected count: "
              << protected_counter << '\n';

    CoarseTokenRing ring(5, 5, {3, 1, 4, 1, 2});
    const auto actions = ring.run_until_legitimate(100'000, 7);
    std::cout << "Coarse token ring stabilized after "
              << actions << " actions\n";

    RingTerminationDetector detector(4, {true, false, false, false});
    detector.go_idle(0);
    (void)detector.try_start_probe();
    (void)detector.try_pass_token(3);
    (void)detector.try_pass_token(2);
    (void)detector.try_pass_token(1);
    std::cout << "Ring termination detected: "
              << std::boolalpha << detector.detected() << '\n';
}
