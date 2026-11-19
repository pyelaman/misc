#include "spanner_prod/coordinator.h"

#include <filesystem>
#include <iostream>
#include <memory>

int main() {
  using namespace spanner_prod;

  const auto root = std::filesystem::temp_directory_path() /
                    "spanner-production-demo";
  std::filesystem::remove_all(root);
  auto lower = std::make_shared<SplitGroup>(101, root / "split-101", 3);
  auto upper = std::make_shared<SplitGroup>(202, root / "split-202", 3);
  auto directory = std::make_shared<RangeDirectory>();
  const std::string boundary = std::string("Accounts") + '\x1f' + "m";
  if (!directory->Add({"", boundary, lower}).ok() ||
      !directory->Add({boundary, std::nullopt, upper}).ok()) {
    std::cerr << "could not create split directory\n";
    return 1;
  }

  auto gps = std::make_shared<SystemTimeSource>(
      "gps-simulated", std::chrono::microseconds(100));
  auto atomic = std::make_shared<SystemTimeSource>(
      "atomic-simulated", std::chrono::microseconds(150));
  auto local = std::make_shared<SystemTimeSource>(
      "local-holdover", std::chrono::microseconds(250));
  auto true_time =
      std::make_shared<QuorumTrueTime>(
          std::vector<std::shared_ptr<ITimeSource>>{gps, atomic, local}, 2);
  TransactionCoordinator coordinator(directory, true_time);

  const Cell alice{"Accounts", "alice", "balance"};
  const Cell zoe{"Accounts", "zoe", "balance"};
  const auto transaction = coordinator.Begin(0, "alice-to-zoe");
  if (!coordinator.BufferWrite(transaction, {alice, std::string("400")}).ok() ||
      !coordinator.BufferWrite(transaction, {zoe, std::string("600")}).ok()) {
    std::cerr << "could not acquire and buffer writes\n";
    return 1;
  }
  const auto committed = coordinator.Commit(transaction);
  if (!committed.status.ok()) {
    std::cerr << "commit failed: " << committed.status.message << '\n';
    return 1;
  }
  std::cout << "committed two-split transfer at "
            << committed.commit_timestamp << '\n';
  std::cout << lower->Fingerprint() << '\n';
  std::cout << upper->Fingerprint() << '\n';
  std::filesystem::remove_all(root);
  return 0;
}

