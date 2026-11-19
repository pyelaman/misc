#include "spanner_lock/coordinator.h"
#include "spanner_lock/lock_table.h"
#include "spanner_lock/replication.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace spanner_lock;

std::atomic<int> failures{0};

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)

TransactionDescriptor Tx(TransactionId id, TransactionAge age,
                         LogicalTick deadline = 0) {
  return TransactionDescriptor{id, age, deadline, "test"};
}

Cell Key(std::string row) {
  return Cell{"Accounts", std::move(row), "balance"};
}

void TestCompatibilityMatrix() {
  CHECK(Compatible(LockMode::kReaderShared, LockMode::kReaderShared));
  CHECK(Compatible(LockMode::kWriterShared, LockMode::kWriterShared));
  CHECK(!Compatible(LockMode::kReaderShared, LockMode::kWriterShared));
  CHECK(!Compatible(LockMode::kExclusive, LockMode::kReaderShared));
  CHECK(!Compatible(LockMode::kWriterSharedTimestamp,
                    LockMode::kWriterSharedTimestamp));
}

void TestSharedReadersAndWriters() {
  LockTable table;
  const Cell reader_key = Key("reader");
  CHECK(table.Acquire(Tx(1, 1), {reader_key, LockMode::kReaderShared}, 1)
            .disposition == AcquireDisposition::kGranted);
  CHECK(table.Acquire(Tx(2, 2), {reader_key, LockMode::kReaderShared}, 2)
            .disposition == AcquireDisposition::kGranted);

  const Cell writer_key = Key("writer");
  CHECK(table.Acquire(Tx(3, 3), {writer_key, LockMode::kWriterShared}, 3)
            .disposition == AcquireDisposition::kGranted);
  CHECK(table.Acquire(Tx(4, 4), {writer_key, LockMode::kWriterShared}, 4)
            .disposition == AcquireDisposition::kGranted);
}

void TestWoundWaitAndDeferredWakeup() {
  LockTable table;
  const Cell cell = Key("wound");
  CHECK(table.Acquire(Tx(20, 20), {cell, LockMode::kExclusive}, 1)
            .disposition == AcquireDisposition::kGranted);
  const auto result =
      table.Acquire(Tx(10, 10), {cell, LockMode::kExclusive}, 2);
  CHECK(result.disposition == AcquireDisposition::kGranted);
  CHECK(result.wounded_transactions == std::vector<TransactionId>{20});
  CHECK(table.IsAborted(20));
  CHECK(table.Holds(10, cell, LockMode::kExclusive));
}

void TestYoungerWaitsAndIsGrantedOnRelease() {
  LockTable table;
  const Cell cell = Key("queue");
  CHECK(table.Acquire(Tx(1, 1), {cell, LockMode::kExclusive}, 1)
            .disposition == AcquireDisposition::kGranted);
  const auto waiting =
      table.Acquire(Tx(2, 2), {cell, LockMode::kExclusive}, 2);
  CHECK(waiting.disposition == AcquireDisposition::kWaiting);
  CHECK(waiting.queue_position == 1);
  const auto grants = table.ReleaseAll(1);
  CHECK(grants.size() == 1);
  CHECK(grants.front().transaction_id == 2);
  CHECK(table.Holds(2, cell, LockMode::kExclusive));
}

void TestUpgradeWaitsForOlderReader() {
  LockTable table;
  const Cell cell = Key("upgrade");
  CHECK(table.Acquire(Tx(1, 1), {cell, LockMode::kReaderShared}, 1)
            .disposition == AcquireDisposition::kGranted);
  CHECK(table.Acquire(Tx(2, 2), {cell, LockMode::kReaderShared}, 2)
            .disposition == AcquireDisposition::kGranted);
  CHECK(table.Acquire(Tx(2, 2), {cell, LockMode::kWriterShared}, 3)
            .disposition == AcquireDisposition::kWaiting);
  const auto grants = table.ReleaseAll(1);
  CHECK(grants.size() == 1);
  CHECK(table.Holds(2, cell, LockMode::kExclusive));
}

void TestStateMachineDeduplicatesCommands() {
  LockShardStateMachine machine;
  const auto descriptor = Tx(1, 1);
  const CommandEnvelope begin{{7, 1}, BeginCommand{descriptor}};
  CHECK(machine.Apply(1, begin).status.ok());
  CHECK(machine.Apply(2, begin).status.ok());
  CHECK(machine.metrics().duplicate_commands == 1);
  CHECK(machine.TransactionStatus(1) == TransactionState::kActive);
}

void TestPreparedHolderCannotBeWounded() {
  LockShardStateMachine machine;
  const Cell cell = Key("prepared-holder");
  const auto younger = Tx(20, 20);
  const auto older = Tx(10, 10);
  CHECK(machine.Apply(1, {{20, 1}, BeginCommand{younger}}).status.ok());
  CHECK(machine
            .Apply(2, {{20, 2}, AcquireCommand{
                                      younger,
                                      {cell, LockMode::kExclusive}, 1}})
            .status.ok());
  CHECK(machine.Apply(3, {{20, 3}, PrepareCommand{20}}).status.ok());
  CHECK(machine.Apply(4, {{10, 1}, BeginCommand{older}}).status.ok());
  const auto acquire = machine.Apply(
      5, {{10, 2},
          AcquireCommand{older, {cell, LockMode::kExclusive}, 2}});
  CHECK(acquire.status.ok());
  CHECK(acquire.acquire->disposition == AcquireDisposition::kWaiting);
  CHECK(acquire.acquire->wounded_transactions.empty());
  CHECK(machine.TransactionStatus(20) == TransactionState::kPrepared);
}

void TestReplicationQuorumAndCatchup() {
  InMemoryReplicatedShard shard(7, 3);
  CHECK(shard.SetReplicaLive(2, false).ok());
  CHECK(shard.Propose({{1, 1}, BeginCommand{Tx(1, 1)}}).status.ok());
  CHECK(shard.SetReplicaLive(2, true).ok());
  CHECK(shard.Fingerprint().find("DIVERGED") == std::string::npos);
  CHECK(shard.SetReplicaLive(1, false).ok());
  CHECK(shard.SetReplicaLive(2, false).ok());
  CHECK(shard.Propose({{2, 1}, BeginCommand{Tx(2, 2)}}).status.code ==
        StatusCode::kUnavailable);
}

class ManualClock final : public ICommitClock {
 public:
  explicit ManualClock(CommitTimestamp now) : now_(now) {}
  TimeInterval Now() const override { return {now_, now_}; }
  void CommitWait(CommitTimestamp timestamp) const override {
    waited_for_.store(timestamp);
  }
  [[nodiscard]] CommitTimestamp waited_for() const { return waited_for_; }

 private:
  CommitTimestamp now_;
  mutable std::atomic<CommitTimestamp> waited_for_{0};
};

class CommitFailingShard final : public IReplicatedShard {
 public:
  explicit CommitFailingShard(ShardId id) : inner_(id) {}

  ShardResult Propose(const CommandEnvelope& envelope) override {
    if (fail_commit_ && std::holds_alternative<CommitCommand>(envelope.command)) {
      fail_commit_ = false;
      return ShardResult{Status::Error(StatusCode::kUnavailable,
                                       "injected commit delivery failure")};
    }
    return inner_.Propose(envelope);
  }
  ShardId id() const override { return inner_.id(); }
  std::string Fingerprint() const override { return inner_.Fingerprint(); }

 private:
  InMemoryReplicatedShard inner_;
  bool fail_commit_{true};
};

std::pair<Cell, Cell> KeysOnDifferentShards(
    const TransactionCoordinator& coordinator) {
  const Cell first = Key("distributed-0");
  for (int i = 1; i < 1000; ++i) {
    const Cell candidate = Key("distributed-" + std::to_string(i));
    if (coordinator.ShardFor(candidate) != coordinator.ShardFor(first)) {
      return {first, candidate};
    }
  }
  std::abort();
}

void TestDistributedCommitAndLockRelease() {
  auto shard_a = std::make_shared<InMemoryReplicatedShard>(11);
  auto shard_b = std::make_shared<InMemoryReplicatedShard>(22);
  auto decisions = std::make_shared<InMemoryDecisionStore>();
  auto clock = std::make_shared<ManualClock>(1000);
  TransactionCoordinator coordinator({shard_a, shard_b}, decisions, clock);
  const auto [first, second] = KeysOnDifferentShards(coordinator);

  const auto transaction = coordinator.Begin(100, "two-shard-transfer");
  const auto acquired = coordinator.Acquire(
      transaction,
      {{first, LockMode::kExclusive}, {second, LockMode::kExclusive}});
  CHECK(acquired.status.ok());
  CHECK(acquired.all_granted);
  const auto committed = coordinator.Commit(transaction);
  CHECK(committed.status.ok());
  CHECK(committed.commit_timestamp == 1000);
  CHECK(clock->waited_for() == 1000);
  CHECK(decisions->Get(transaction.descriptor.id)->decision ==
        CoordinatorDecision::kCommit);

  const auto follower = coordinator.Begin();
  const auto reacquired = coordinator.Acquire(
      follower,
      {{first, LockMode::kExclusive}, {second, LockMode::kExclusive}});
  CHECK(reacquired.status.ok());
  CHECK(reacquired.all_granted);
}

void TestDurableDecisionCanBeRecovered() {
  auto shard = std::make_shared<CommitFailingShard>(33);
  auto decisions = std::make_shared<InMemoryDecisionStore>();
  auto clock = std::make_shared<ManualClock>(2000);
  TransactionCoordinator coordinator({shard}, decisions, clock);

  const auto transaction = coordinator.Begin();
  CHECK(coordinator
            .Acquire(transaction, {{Key("recover"), LockMode::kExclusive}})
            .all_granted);
  const auto committed = coordinator.Commit(transaction);
  CHECK(committed.status.code == StatusCode::kInDoubt);
  CHECK(decisions->Get(transaction.descriptor.id)->decision ==
        CoordinatorDecision::kCommit);
  CHECK(coordinator.Recover(transaction.descriptor.id).ok());
}

void TestDecisionIsImmutable() {
  InMemoryDecisionStore decisions;
  const DecisionRecord commit{77, CoordinatorDecision::kCommit, 123, {1, 2},
                              {}};
  CHECK(decisions.PutIfAbsent(commit).ok());
  CHECK(decisions.PutIfAbsent(commit).ok());
  CHECK(decisions
            .PutIfAbsent(
                {77, CoordinatorDecision::kAbort, 0, {1, 2}, "too late"})
            .code == StatusCode::kFailedPrecondition);
}

void TestConcurrentCoordinatorClients() {
  auto shard_a = std::make_shared<InMemoryReplicatedShard>(51);
  auto shard_b = std::make_shared<InMemoryReplicatedShard>(52);
  auto decisions = std::make_shared<InMemoryDecisionStore>();
  auto clock = std::make_shared<ManualClock>(4000);
  TransactionCoordinator coordinator({shard_a, shard_b}, decisions, clock);

  std::mutex result_mutex;
  std::vector<CommitTimestamp> timestamps;
  std::vector<std::thread> clients;
  for (int i = 0; i < 8; ++i) {
    clients.emplace_back([&, i] {
      const auto transaction = coordinator.Begin();
      const auto acquired = coordinator.Acquire(
          transaction,
          {{Key("thread-" + std::to_string(i)), LockMode::kExclusive}});
      const auto committed = coordinator.Commit(transaction);
      CHECK(acquired.status.ok() && acquired.all_granted);
      CHECK(committed.status.ok());
      std::lock_guard lock(result_mutex);
      timestamps.push_back(committed.commit_timestamp);
    });
  }
  for (auto& client : clients) {
    client.join();
  }
  CHECK(timestamps.size() == 8);
  CHECK(std::set<CommitTimestamp>(timestamps.begin(), timestamps.end()).size() ==
        8);
}

void TestActiveDeadlineAbortsTransaction() {
  auto shard = std::make_shared<InMemoryReplicatedShard>(44);
  auto decisions = std::make_shared<InMemoryDecisionStore>();
  auto clock = std::make_shared<ManualClock>(3000);
  TransactionCoordinator coordinator({shard}, decisions, clock);

  const auto transaction = coordinator.Begin(5, "deadline");
  CHECK(coordinator
            .Acquire(transaction, {{Key("deadline"), LockMode::kExclusive}})
            .all_granted);
  CHECK(coordinator.AdvanceTick(5).ok());
  CHECK(coordinator.Commit(transaction).status.code ==
        StatusCode::kFailedPrecondition);
  CHECK(decisions->Get(transaction.descriptor.id)->decision ==
        CoordinatorDecision::kAbort);
}

}  // namespace

int main() {
  TestCompatibilityMatrix();
  TestSharedReadersAndWriters();
  TestWoundWaitAndDeferredWakeup();
  TestYoungerWaitsAndIsGrantedOnRelease();
  TestUpgradeWaitsForOlderReader();
  TestStateMachineDeduplicatesCommands();
  TestPreparedHolderCannotBeWounded();
  TestReplicationQuorumAndCatchup();
  TestDistributedCommitAndLockRelease();
  TestDurableDecisionCanBeRecovered();
  TestDecisionIsImmutable();
  TestConcurrentCoordinatorClients();
  TestActiveDeadlineAbortsTransaction();

  if (failures.load() != 0) {
    std::cerr << failures.load() << " test check(s) failed\n";
    return 1;
  }
  std::cout << "all lock-manager tests passed\n";
  return 0;
}
