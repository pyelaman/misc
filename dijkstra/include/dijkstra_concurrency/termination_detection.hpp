#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dijkstra::concurrency {

class TreeTerminationDetector {
public:
    struct Message {
        std::uint64_t id;
        std::size_t source;
        std::size_t destination;
    };

    explicit TreeTerminationDetector(
        std::size_t process_count,
        std::size_t leader = 0);

    TreeTerminationDetector(const TreeTerminationDetector&) = delete;
    TreeTerminationDetector& operator=(const TreeTerminationDetector&) = delete;

    // Computation messages and control acknowledgements are modeled
    // separately. send() accounts for an unacknowledged message; receive()
    // either retains it as the receiver's up edge or acknowledges it.
    [[nodiscard]] Message send(
        std::size_t source,
        std::size_t destination);
    void receive(const Message& message);
    void go_idle(std::size_t process);
    void activate_leader();

    [[nodiscard]] bool detected() const;
    [[nodiscard]] bool is_active(std::size_t process) const;
    [[nodiscard]] bool is_neutral(std::size_t process) const;
    [[nodiscard]] std::size_t outstanding_messages(
        std::size_t process) const;

private:
    struct PendingMessage {
        std::size_t source;
        std::size_t destination;
    };

    void check_id(std::size_t process) const;
    [[nodiscard]] bool is_neutral_unlocked(std::size_t process) const;
    void acknowledge_unlocked(std::size_t source);
    void settle_unlocked(std::size_t process);

    std::size_t process_count_;
    std::size_t leader_;
    mutable std::mutex mutex_;
    std::vector<bool> active_;
    std::vector<std::size_t> outstanding_;
    std::vector<std::optional<std::size_t>> parent_;
    std::unordered_map<std::uint64_t, PendingMessage> pending_;
    std::uint64_t next_message_id_ = 1;
};

class RingTerminationDetector {
public:
    enum class Color {
        white,
        black,
    };

    explicit RingTerminationDetector(
        std::size_t process_count,
        std::vector<bool> initially_active = {});

    RingTerminationDetector(const RingTerminationDetector&) = delete;
    RingTerminationDetector& operator=(const RingTerminationDetector&) = delete;

    void go_idle(std::size_t process);
    void send_message(std::size_t source, std::size_t destination);
    [[nodiscard]] bool try_start_probe();
    [[nodiscard]] bool try_pass_token(std::size_t process);

    [[nodiscard]] bool detected() const;
    [[nodiscard]] bool is_active(std::size_t process) const;
    [[nodiscard]] Color process_color(std::size_t process) const;
    [[nodiscard]] Color token_color() const;
    [[nodiscard]] std::size_t token_location() const;

private:
    void check_id(std::size_t process) const;

    std::size_t process_count_;
    mutable std::mutex mutex_;
    std::vector<bool> active_;
    std::vector<Color> process_color_;
    std::size_t token_location_ = 0;
    Color token_color_ = Color::black;
};

} // namespace dijkstra::concurrency
