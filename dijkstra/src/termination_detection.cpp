#include "dijkstra_concurrency/termination_detection.hpp"

#include <stdexcept>
#include <utility>

namespace dijkstra::concurrency {

TreeTerminationDetector::TreeTerminationDetector(
    const std::size_t process_count,
    const std::size_t leader)
    : process_count_(process_count),
      leader_(leader),
      active_(process_count, false),
      outstanding_(process_count, 0),
      parent_(process_count) {
    if (process_count_ < 2) {
        throw std::invalid_argument(
            "tree termination detector requires at least two processes");
    }
    check_id(leader_);
    active_[leader_] = true;
}

void TreeTerminationDetector::check_id(const std::size_t process) const {
    if (process >= process_count_) {
        throw std::out_of_range("process id is outside termination detector");
    }
}

bool TreeTerminationDetector::is_neutral_unlocked(
    const std::size_t process) const {
    return !active_[process] &&
           outstanding_[process] == 0 &&
           !parent_[process].has_value();
}

TreeTerminationDetector::Message TreeTerminationDetector::send(
    const std::size_t source,
    const std::size_t destination) {
    check_id(source);
    check_id(destination);
    if (source == destination) {
        throw std::invalid_argument("tree detector message must cross an edge");
    }

    std::lock_guard lock(mutex_);
    if (!active_[source]) {
        throw std::logic_error("only an active process may send a message");
    }
    if (destination == leader_) {
        throw std::logic_error(
            "tree detector assumes that the leader has only outgoing edges");
    }

    const std::uint64_t id = next_message_id_++;
    pending_.emplace(id, PendingMessage{source, destination});
    ++outstanding_[source];
    return Message{id, source, destination};
}

void TreeTerminationDetector::acknowledge_unlocked(
    const std::size_t source) {
    if (outstanding_[source] == 0) {
        throw std::logic_error("acknowledgement has no matching message");
    }
    --outstanding_[source];
}

void TreeTerminationDetector::settle_unlocked(std::size_t process) {
    while (process != leader_ &&
           !active_[process] &&
           outstanding_[process] == 0 &&
           parent_[process].has_value()) {
        const std::size_t parent = *parent_[process];
        parent_[process].reset();
        acknowledge_unlocked(parent);
        process = parent;
    }
}

void TreeTerminationDetector::receive(const Message& message) {
    check_id(message.source);
    check_id(message.destination);
    std::lock_guard lock(mutex_);

    const auto found = pending_.find(message.id);
    if (found == pending_.end() ||
        found->second.source != message.source ||
        found->second.destination != message.destination) {
        throw std::logic_error("unknown or already delivered message");
    }

    const bool was_neutral =
        is_neutral_unlocked(message.destination);
    active_[message.destination] = true;
    if (was_neutral) {
        // Retain this acknowledgement as the process's up edge.
        parent_[message.destination] = message.source;
    } else {
        // The receiver is already represented in the outstanding tree, so
        // this additional message can be acknowledged immediately.
        acknowledge_unlocked(message.source);
    }
    pending_.erase(found);
}

void TreeTerminationDetector::go_idle(const std::size_t process) {
    check_id(process);
    std::lock_guard lock(mutex_);
    active_[process] = false;
    settle_unlocked(process);
}

void TreeTerminationDetector::activate_leader() {
    std::lock_guard lock(mutex_);
    active_[leader_] = true;
}

bool TreeTerminationDetector::detected() const {
    std::lock_guard lock(mutex_);
    return is_neutral_unlocked(leader_);
}

bool TreeTerminationDetector::is_active(
    const std::size_t process) const {
    check_id(process);
    std::lock_guard lock(mutex_);
    return active_[process];
}

bool TreeTerminationDetector::is_neutral(
    const std::size_t process) const {
    check_id(process);
    std::lock_guard lock(mutex_);
    return is_neutral_unlocked(process);
}

std::size_t TreeTerminationDetector::outstanding_messages(
    const std::size_t process) const {
    check_id(process);
    std::lock_guard lock(mutex_);
    return outstanding_[process];
}

RingTerminationDetector::RingTerminationDetector(
    const std::size_t process_count,
    std::vector<bool> initially_active)
    : process_count_(process_count),
      process_color_(process_count, Color::white) {
    if (process_count_ < 2) {
        throw std::invalid_argument(
            "ring termination detector requires at least two processes");
    }
    if (initially_active.empty()) {
        initially_active.assign(process_count_, false);
    }
    if (initially_active.size() != process_count_) {
        throw std::invalid_argument(
            "initial active vector has the wrong size");
    }
    active_ = std::move(initially_active);
}

void RingTerminationDetector::check_id(
    const std::size_t process) const {
    if (process >= process_count_) {
        throw std::out_of_range("process id is outside termination ring");
    }
}

void RingTerminationDetector::go_idle(const std::size_t process) {
    check_id(process);
    std::lock_guard lock(mutex_);
    active_[process] = false;
}

void RingTerminationDetector::send_message(
    const std::size_t source,
    const std::size_t destination) {
    check_id(source);
    check_id(destination);
    if (source == destination) {
        throw std::invalid_argument("ring computation message must cross an edge");
    }
    std::lock_guard lock(mutex_);
    if (!active_[source]) {
        throw std::logic_error("only an active process may send a message");
    }
    active_[destination] = true;
    process_color_[source] = Color::black;
}

bool RingTerminationDetector::try_start_probe() {
    std::lock_guard lock(mutex_);
    if (token_location_ != 0 ||
        (token_color_ != Color::black &&
         process_color_[0] != Color::black)) {
        return false;
    }
    token_color_ = Color::white;
    process_color_[0] = Color::white;
    token_location_ = process_count_ - 1;
    return true;
}

bool RingTerminationDetector::try_pass_token(
    const std::size_t process) {
    check_id(process);
    if (process == 0) {
        throw std::invalid_argument("process 0 starts rather than passes probes");
    }

    std::lock_guard lock(mutex_);
    if (token_location_ != process || active_[process]) {
        return false;
    }
    token_location_ = process - 1;
    if (process_color_[process] == Color::black) {
        token_color_ = Color::black;
        process_color_[process] = Color::white;
    }
    return true;
}

bool RingTerminationDetector::detected() const {
    std::lock_guard lock(mutex_);
    return token_location_ == 0 &&
           !active_[0] &&
           token_color_ == Color::white &&
           process_color_[0] == Color::white;
}

bool RingTerminationDetector::is_active(
    const std::size_t process) const {
    check_id(process);
    std::lock_guard lock(mutex_);
    return active_[process];
}

RingTerminationDetector::Color RingTerminationDetector::process_color(
    const std::size_t process) const {
    check_id(process);
    std::lock_guard lock(mutex_);
    return process_color_[process];
}

RingTerminationDetector::Color
RingTerminationDetector::token_color() const {
    std::lock_guard lock(mutex_);
    return token_color_;
}

std::size_t RingTerminationDetector::token_location() const {
    std::lock_guard lock(mutex_);
    return token_location_;
}

} // namespace dijkstra::concurrency
