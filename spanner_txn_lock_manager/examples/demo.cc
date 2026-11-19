#include "spanner_lock/coordinator.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main() {
  using namespace spanner_lock;

  auto shard_a = std::make_shared<InMemoryReplicatedShard>(101, 3);
  auto shard_b = std::make_shared<InMemoryReplicatedShard>(202, 3);
  auto decisions = std::make_shared<InMemoryDecisionStore>();
  auto clock = std::make_shared<SystemUncertaintyClock>();
  TransactionCoordinator coordinator({shard_a, shard_b}, decisions, clock);

  Cell checking{"Accounts", "alice", "balance"};
  Cell savings{"Accounts", "bob", "balance"};
  while (coordinator.ShardFor(checking) == coordinator.ShardFor(savings)) {
    savings.row.push_back('x');
  }

  const auto transfer = coordinator.Begin(100, "alice-to-bob");
  const std::vector<LockRequest> locks{
      {checking, LockMode::kExclusive},
      {savings, LockMode::kExclusive},
  };
  const auto acquired = coordinator.Acquire(transfer, locks);
  if (!acquired.status.ok() || !acquired.all_granted) {
    std::cerr << "could not acquire transfer locks: "
              << acquired.status.message << '\n';
    return 1;
  }

  std::cout << "transaction " << transfer.descriptor.id
            << " holds cells on shards " << coordinator.ShardFor(checking)
            << " and " << coordinator.ShardFor(savings) << '\n';

  const auto committed = coordinator.Commit(transfer);
  if (!committed.status.ok()) {
    std::cerr << "commit failed: " << committed.status.message << '\n';
    return 1;
  }
  std::cout << "durable commit decision at timestamp "
            << committed.commit_timestamp << " (after commit wait)\n";
  std::cout << shard_a->Fingerprint() << '\n';
  std::cout << shard_b->Fingerprint() << '\n';
  return 0;
}
