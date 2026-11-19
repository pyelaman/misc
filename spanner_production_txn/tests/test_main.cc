#include "spanner_prod/codec.h"
#include "spanner_prod/coordinator.h"
#include "spanner_prod/lock_manager.h"
#include "spanner_prod/mvcc_store.h"
#include "spanner_prod/persistent_replica.h"
#include "spanner_prod/split_group.h"
#include "spanner_prod/true_time.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace spanner_prod;

std::atomic<int> failures{0};

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    std::cerr << file << ':' << line << ": check failed: " << expression
              << '\n';
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __FILE__, __LINE__)

Cell Key(std::string row) {
  return {"Accounts", std::move(row), "balance"};
}

std::filesystem::path TestRoot(std::string_view name) {
  auto root = std::filesystem::temp_directory_path() /
              ("spanner-prod-" + std::to_string(::getpid()) + "-" +
               std::string(name));
  std::filesystem::remove_all(root);
  return root;
}

TransactionDescriptor Tx(TransactionId id, TransactionPriority priority,
                         SplitId coordinator = 1) {
  return {id, priority, 0, coordinator, "test"};
}

void TestCodecRoundTrip() {
  PreparedTransaction prepared{
      Tx(9, 4), 123, {{Key("alice"), LockMode::kExclusive}},
      {{Key("alice"), std::string("500")}}};
  LogEntry original{7, 11, 99, PrepareCommand{prepared}};
  const std::string encoded = EncodeLogEntry(original);
  auto decoded = DecodeLogEntry(encoded);
  CHECK(decoded.ok());
  CHECK(decoded.value() == original);
  CHECK(Checksum(encoded) != 0);
}

void TestMvccVersionsAndTombstones() {
  MvccStore store;
  const Cell cell = Key("versioned");
  CHECK(store.Apply(10, {{cell, std::string("one")}}).ok());
  CHECK(store.Apply(20, {{cell, std::string("two")}}).ok());
  CHECK(store.Apply(30, {{cell, std::nullopt}}).ok());
  CHECK(!store.Read(cell, 5).value().has_value());
  CHECK(store.Read(cell, 10).value() == std::optional<std::string>{"one"});
  CHECK(store.Read(cell, 25).value() == std::optional<std::string>{"two"});
  CHECK(!store.Read(cell, 30).value().has_value());

  const Cell untouched = Key("must-remain-absent");
  CHECK(store
            .Apply(20, {{untouched, std::string("partial")},
                        {cell, std::string("conflict")}})
            .code == StatusCode::kFailedPrecondition);
  CHECK(!store.Read(untouched, 20).value().has_value());

  CHECK(store
            .Apply(40, {{untouched, std::string("a")},
                        {untouched, std::string("b")}})
            .code == StatusCode::kFailedPrecondition);
  CHECK(!store.Read(untouched, 40).value().has_value());
}

void TestPreparedLockCannotBeWounded() {
  LockManager locks;
  const Cell cell = Key("protected");
  CHECK(locks.Acquire(Tx(20, 20), {cell, LockMode::kExclusive}, 1)
            .disposition == AcquireDisposition::kGranted);
  CHECK(locks.ProtectPrepared(20).ok());
  const auto older =
      locks.Acquire(Tx(10, 10), {cell, LockMode::kExclusive}, 2);
  CHECK(older.disposition == AcquireDisposition::kWaiting);
  CHECK(older.wounded.empty());
  locks.Release(20);
  CHECK(locks.Holds(10, {cell, LockMode::kExclusive}));
}

void TestPersistentQuorumRecovery() {
  const auto root = TestRoot("quorum");
  {
    PersistentConsensusGroup group(1, root, 3);
    CHECK(group.Propose(1, AdvanceSafeTimeCommand{100}).status.ok());
    group.FailAfterQuorumAppendOnce();
    CHECK(group.Propose(2, AdvanceSafeTimeCommand{200}).status.code ==
          StatusCode::kInDoubt);
    CHECK(group.RecoverPending().ok());
    CHECK(group.safe_time() == 200);
    CHECK(group.SetReplicaLive(2, false).ok());
    CHECK(group.Propose(3, AdvanceSafeTimeCommand{300}).status.ok());
    CHECK(group.SetReplicaLive(2, true).ok());
    CHECK(group.Fingerprint().find("DIVERGED") == std::string::npos);
    CHECK(group.SetReplicaLive(1, false).ok());
    CHECK(group.SetReplicaLive(2, false).ok());
    CHECK(group.Propose(4, AdvanceSafeTimeCommand{400}).status.code ==
          StatusCode::kUnavailable);
  }
  {
    PersistentConsensusGroup recovered(1, root, 3);
    CHECK(recovered.safe_time() == 300);
    CHECK(recovered.Fingerprint().find("DIVERGED") == std::string::npos);
  }
  std::filesystem::remove_all(root);
}

void TestCommittedApplicationErrorDoesNotPoisonWal() {
  const auto root = TestRoot("committed-error");
  {
    PersistentConsensusGroup group(1, root, 3);
    CHECK(group.Propose(1, CommitCommand{999, 100}).status.code ==
          StatusCode::kNotFound);
    CHECK(group.Propose(2, AdvanceSafeTimeCommand{200}).status.ok());
  }
  {
    PersistentConsensusGroup recovered(1, root, 3);
    CHECK(recovered.safe_time() == 200);
  }
  std::filesystem::remove_all(root);
}

void TestLeaderRestoresPreparedLocks() {
  const auto root = TestRoot("prepared");
  SplitGroup group(1, root, 3);
  const Cell cell = Key("recover-lock");
  CHECK(group.Begin(Tx(20, 20)).ok());
  CHECK(group.Acquire(20, {cell, LockMode::kWriterShared})
            .acquire.disposition == AcquireDisposition::kGranted);
  CHECK(group.BufferMutation(20, {cell, std::string("durable")}).ok());
  auto prepared = group.Prepare(20, 10);
  CHECK(prepared.ok());
  CHECK(group.ElectLeader(1).ok());
  CHECK(group.Begin(Tx(10, 10)).ok());
  CHECK(group.Acquire(10, {cell, LockMode::kExclusive})
            .acquire.disposition == AcquireDisposition::kWaiting);
  CHECK(group.Commit(20, prepared.value()).ok());
  CHECK(group.Acquire(10, {cell, LockMode::kExclusive})
            .acquire.disposition == AcquireDisposition::kGranted);
  CHECK(group.AdvanceSafeTime(prepared.value()).ok());
  CHECK(group.ReadAt(cell, prepared.value()).value() ==
        std::optional<std::string>{"durable"});
  std::filesystem::remove_all(root);
}

class FixedSource final : public ITimeSource {
 public:
  FixedSource(std::string name, TimeInterval interval, bool healthy = true)
      : name_(std::move(name)), interval_(interval), healthy_(healthy) {}
  TimeSample Sample() const override {
    return {healthy_ ? Status::Ok()
                     : Status::Error(StatusCode::kUnavailable, "failed"),
            interval_, name_};
  }

 private:
  std::string name_;
  TimeInterval interval_;
  bool healthy_;
};

class AutoAdvancingTrueTime final : public ITrueTime {
 public:
  explicit AutoAdvancingTrueTime(Timestamp now) : now_(now) {}
  StatusOr<TimeInterval> Now() const override {
    const Timestamp value = now_.load();
    return TimeInterval{value, value + 5};
  }
  Status CommitWait(Timestamp timestamp,
                    std::chrono::milliseconds timeout) const override {
    (void)timeout;
    Timestamp observed = now_.load();
    while (observed <= timestamp &&
           !now_.compare_exchange_weak(observed, timestamp + 1)) {
    }
    return Status::Ok();
  }

 private:
  mutable std::atomic<Timestamp> now_;
};

void TestTrueTimeQuorum() {
  auto a = std::make_shared<FixedSource>("gps-a", TimeInterval{90, 110});
  auto b = std::make_shared<FixedSource>("gps-b", TimeInterval{95, 105});
  auto c = std::make_shared<FixedSource>("atomic", TimeInterval{100, 120});
  QuorumTrueTime clock({a, b, c}, 2);
  auto now = clock.Now();
  CHECK(now.ok());
  CHECK(now.value().earliest <= 100 && now.value().latest >= 100);

  auto failed = std::make_shared<FixedSource>("failed", TimeInterval{}, false);
  QuorumTrueTime unavailable({a, failed}, 2);
  CHECK(unavailable.Now().status().code == StatusCode::kClockUncertain);
}

std::shared_ptr<RangeDirectory> TwoSplitDirectory(
    const std::shared_ptr<SplitGroup>& lower,
    const std::shared_ptr<SplitGroup>& upper) {
  auto directory = std::make_shared<RangeDirectory>();
  const std::string boundary = std::string("Accounts") + '\x1f' + "m";
  CHECK(directory->Add({"", boundary, lower}).ok());
  CHECK(directory->Add({boundary, std::nullopt, upper}).ok());
  return directory;
}

void TestExternallyOrderedMultiSplitTransactions() {
  const auto root = TestRoot("coordinator");
  auto lower = std::make_shared<SplitGroup>(1, root / "lower", 3);
  auto upper = std::make_shared<SplitGroup>(2, root / "upper", 3);
  auto directory = TwoSplitDirectory(lower, upper);
  auto clock = std::make_shared<AutoAdvancingTrueTime>(1000);
  TransactionCoordinator coordinator(directory, clock);

  const Cell alice = Key("alice");
  const Cell zoe = Key("zoe");
  const auto first = coordinator.Begin(0, "first-transfer");
  CHECK(coordinator.BufferWrite(first, {alice, std::string("400")}).ok());
  CHECK(coordinator.BufferWrite(first, {zoe, std::string("600")}).ok());
  const auto committed_first = coordinator.Commit(first);
  CHECK(committed_first.status.ok());

  const auto second = coordinator.Begin(0, "second-transfer");
  CHECK(coordinator.BufferWrite(second, {alice, std::string("350")}).ok());
  CHECK(coordinator.BufferWrite(second, {zoe, std::string("650")}).ok());
  const auto committed_second = coordinator.Commit(second);
  CHECK(committed_second.status.ok());
  CHECK(committed_first.commit_timestamp < committed_second.commit_timestamp);

  CHECK(coordinator
            .SnapshotRead(alice, committed_first.commit_timestamp)
            .value() == std::optional<std::string>{"400"});
  CHECK(coordinator
            .SnapshotRead(alice, committed_second.commit_timestamp)
            .value() == std::optional<std::string>{"350"});
  CHECK(coordinator.StrongRead(zoe).value() ==
        std::optional<std::string>{"650"});
  std::filesystem::remove_all(root);
}

void TestConcurrentCoordinatorClients() {
  const auto root = TestRoot("concurrent-clients");
  auto group = std::make_shared<SplitGroup>(1, root / "split", 3);
  auto directory = std::make_shared<RangeDirectory>();
  CHECK(directory->Add({"", std::nullopt, group}).ok());
  auto clock = std::make_shared<AutoAdvancingTrueTime>(2000);
  TransactionCoordinator coordinator(directory, clock);

  constexpr int kClients = 8;
  std::atomic<int> committed{0};
  std::vector<std::thread> clients;
  for (int client = 0; client < kClients; ++client) {
    clients.emplace_back([&, client] {
      const auto transaction = coordinator.Begin(0, "concurrent-client");
      const Cell cell = Key("client-" + std::to_string(client));
      if (!coordinator
               .BufferWrite(transaction,
                            {cell, std::to_string(client * 100)})
               .ok()) {
        return;
      }
      if (coordinator.Commit(transaction).status.ok()) {
        ++committed;
      }
    });
  }
  for (auto& client : clients) {
    client.join();
  }
  CHECK(committed.load() == kClients);
  for (int client = 0; client < kClients; ++client) {
    CHECK(coordinator.StrongRead(Key("client-" + std::to_string(client)))
              .value() ==
          std::optional<std::string>{std::to_string(client * 100)});
  }
  std::filesystem::remove_all(root);
}

void TestColdCoordinatorRecoveryFromDurableDecision() {
  const auto root = TestRoot("cold-recovery");
  const Cell cell = Key("cold");
  Timestamp prepare_timestamp = 0;
  {
    auto group = std::make_shared<SplitGroup>(1, root / "split", 3);
    CHECK(group->Begin(Tx(70, 70)).ok());
    CHECK(group->Acquire(70, {cell, LockMode::kWriterShared})
              .acquire.disposition == AcquireDisposition::kGranted);
    CHECK(group->BufferMutation(70, {cell, std::string("recovered")}).ok());
    auto prepared = group->Prepare(70, 500);
    CHECK(prepared.ok());
    prepare_timestamp = prepared.value();
    CHECK(group
              ->RecordDecision({70, DecisionKind::kCommit,
                                prepare_timestamp, {1}, {}})
              .ok());
  }

  // Recreate both the split group and coordinator. The only surviving state
  // is the replica WAL set and the small recovery token.
  {
    auto group = std::make_shared<SplitGroup>(1, root / "split", 3);
    auto directory = std::make_shared<RangeDirectory>();
    CHECK(directory->Add({"", std::nullopt, group}).ok());
    auto clock =
        std::make_shared<AutoAdvancingTrueTime>(prepare_timestamp);
    TransactionCoordinator restarted(directory, clock);
    CHECK(restarted.Recover(RecoveryToken{70, 1}).ok());
    CHECK(restarted.SnapshotRead(cell, prepare_timestamp).value() ==
          std::optional<std::string>{"recovered"});
  }
  std::filesystem::remove_all(root);
}

void TestColdRecoveryWithoutDecisionAborts() {
  const auto root = TestRoot("cold-abort");
  const Cell cell = Key("undecided");
  {
    auto group = std::make_shared<SplitGroup>(1, root / "split", 3);
    CHECK(group->Begin(Tx(80, 80)).ok());
    CHECK(group->Acquire(80, {cell, LockMode::kWriterShared})
              .acquire.disposition == AcquireDisposition::kGranted);
    CHECK(group->BufferMutation(80, {cell, std::string("must-not-commit")})
              .ok());
    CHECK(group->Prepare(80, 700).ok());
  }
  {
    auto group = std::make_shared<SplitGroup>(1, root / "split", 3);
    auto directory = std::make_shared<RangeDirectory>();
    CHECK(directory->Add({"", std::nullopt, group}).ok());
    auto clock = std::make_shared<AutoAdvancingTrueTime>(800);
    TransactionCoordinator restarted(directory, clock);
    CHECK(restarted.Recover(RecoveryToken{80, 1}).ok());
    const auto decision = group->Decision(80);
    CHECK(decision.has_value());
    CHECK(decision->kind == DecisionKind::kAbort);
    CHECK(group->Prepared().empty());
    CHECK(group->AdvanceSafeTime(800).ok());
    CHECK(!group->ReadAt(cell, 800).value().has_value());
  }
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  TestCodecRoundTrip();
  TestMvccVersionsAndTombstones();
  TestPreparedLockCannotBeWounded();
  TestPersistentQuorumRecovery();
  TestCommittedApplicationErrorDoesNotPoisonWal();
  TestLeaderRestoresPreparedLocks();
  TestTrueTimeQuorum();
  TestExternallyOrderedMultiSplitTransactions();
  TestConcurrentCoordinatorClients();
  TestColdCoordinatorRecoveryFromDurableDecision();
  TestColdRecoveryWithoutDecisionAborts();

  if (failures.load() != 0) {
    std::cerr << failures.load() << " check(s) failed\n";
    return 1;
  }
  std::cout << "all production-shaped Spanner tests passed\n";
  return 0;
}
