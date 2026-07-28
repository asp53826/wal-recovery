#include "wal/wal.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
int assertions = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++assertions;                                                            \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "       \
                << #condition << "\n";                                      \
    }                                                                        \
  } while (false)

std::string temp_path(const std::string& test_name) {
  return (std::filesystem::temp_directory_path() /
          ("wal-recovery-" + test_name + "-" +
           std::to_string(static_cast<long long>(::getpid())) + ".wal"))
      .string();
}

void remove_if_present(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

void test_crc32_reference_vector() {
  const std::string input = "123456789";
  CHECK(wal::crc32(reinterpret_cast<const std::uint8_t*>(input.data()),
                   input.size()) == 0xCBF43926U);
}

void test_committed_transaction_recovers_atomically() {
  const std::string path = temp_path("commit");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    const auto tx = log.begin();
    log.put(tx, "alpha", "1");
    log.put(tx, "beta", "2");
    log.commit(tx);
  }
  const auto recovered = wal::WriteAheadLog::recover(path);
  CHECK(recovered.state.size() == 2);
  CHECK(recovered.state.at("alpha") == "1");
  CHECK(recovered.state.at("beta") == "2");
  CHECK(recovered.committed_transactions == 1);
  CHECK(recovered.discarded_transactions == 0);
  CHECK(!recovered.tail_damaged);
  remove_if_present(path);
}

void test_uncommitted_transaction_is_invisible() {
  const std::string path = temp_path("uncommitted");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    const auto tx = log.begin();
    log.put(tx, "ghost", "not-visible");
  }
  const auto recovered = wal::WriteAheadLog::recover(path);
  CHECK(recovered.state.empty());
  CHECK(recovered.committed_transactions == 0);
  CHECK(recovered.discarded_transactions == 1);
  CHECK(!recovered.tail_damaged);
  remove_if_present(path);
}

void test_delete_replays() {
  const std::string path = temp_path("delete");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    auto tx = log.begin();
    log.put(tx, "key", "value");
    log.commit(tx);
    tx = log.begin();
    log.erase(tx, "key");
    log.commit(tx);
  }
  const auto recovered = wal::WriteAheadLog::recover(path);
  CHECK(recovered.state.empty());
  CHECK(recovered.committed_transactions == 2);
  remove_if_present(path);
}

void test_transaction_ids_continue_after_restart() {
  const std::string path = temp_path("ids");
  remove_if_present(path);
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  {
    wal::WriteAheadLog log(path);
    first = log.begin();
    log.commit(first);
  }
  {
    wal::WriteAheadLog log(path);
    second = log.begin();
    log.commit(second);
  }
  CHECK(first == 1);
  CHECK(second == 2);
  remove_if_present(path);
}

void test_truncated_tail_stops_before_partial_transaction() {
  const std::string path = temp_path("truncate");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    auto tx = log.begin();
    log.put(tx, "durable", "yes");
    log.commit(tx);
    tx = log.begin();
    log.put(tx, "partial", "no");
    log.commit(tx);
  }
  const auto complete = wal::WriteAheadLog::recover(path);
  const auto file_size = std::filesystem::file_size(path);
  CHECK(file_size == complete.valid_bytes);
  std::filesystem::resize_file(path, file_size - 3);

  const auto damaged = wal::WriteAheadLog::recover(path, false);
  CHECK(damaged.tail_damaged);
  CHECK(damaged.state.size() == 1);
  CHECK(damaged.state.at("durable") == "yes");
  CHECK(damaged.state.find("partial") == damaged.state.end());

  const auto repaired = wal::WriteAheadLog::recover(path, true);
  CHECK(repaired.tail_damaged);
  CHECK(std::filesystem::file_size(path) == repaired.valid_bytes);
  remove_if_present(path);
}

void test_checksum_corruption_is_not_replayed() {
  const std::string path = temp_path("checksum");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    auto tx = log.begin();
    log.put(tx, "first", "safe");
    log.commit(tx);
    tx = log.begin();
    log.put(tx, "second", "corrupt-me");
    log.commit(tx);
  }
  const auto original = wal::WriteAheadLog::recover(path);
  CHECK(original.state.size() == 2);

  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekg(-8, std::ios::end);
  char byte = 0;
  file.read(&byte, 1);
  byte ^= 0x5A;
  file.seekp(-8, std::ios::end);
  file.write(&byte, 1);
  file.flush();

  const auto corrupted = wal::WriteAheadLog::recover(path);
  CHECK(corrupted.tail_damaged);
  CHECK(corrupted.state.size() == 1);
  CHECK(corrupted.state.at("first") == "safe");
  CHECK(corrupted.state.find("second") == corrupted.state.end());
  remove_if_present(path);
}

void test_every_truncation_of_a_transaction_is_atomic() {
  const std::string source_path = temp_path("all-cuts-source");
  const std::string cut_path = temp_path("all-cuts-candidate");
  remove_if_present(source_path);
  remove_if_present(cut_path);

  {
    wal::WriteAheadLog log(source_path);
    const auto tx = log.begin();
    log.put(tx, "durable", "yes");
    log.commit(tx);
  }
  const std::uint64_t durable_boundary =
      wal::WriteAheadLog::recover(source_path).valid_bytes;
  {
    wal::WriteAheadLog log(source_path);
    const auto tx = log.begin();
    log.put(tx, "must-be-atomic", "complete-or-absent");
    log.commit(tx);
  }

  std::ifstream source(source_path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(source)),
                                std::istreambuf_iterator<char>());
  CHECK(bytes.size() > durable_boundary);

  for (std::size_t cut = static_cast<std::size_t>(durable_boundary) + 1;
       cut < bytes.size(); ++cut) {
    {
      std::ofstream candidate(cut_path,
                              std::ios::binary | std::ios::trunc);
      candidate.write(bytes.data(), static_cast<std::streamsize>(cut));
    }
    const auto recovered = wal::WriteAheadLog::recover(cut_path);
    CHECK(recovered.state.size() == 1);
    CHECK(recovered.state.at("durable") == "yes");
    CHECK(recovered.state.find("must-be-atomic") == recovered.state.end());
  }

  remove_if_present(source_path);
  remove_if_present(cut_path);
}

void test_group_commit_recovers_every_transaction() {
  const std::string path = temp_path("group-commit");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    std::vector<std::uint64_t> transactions;
    for (int index = 0; index < 32; ++index) {
      const auto transaction = log.begin();
      log.put(transaction, "key-" + std::to_string(index),
              "value-" + std::to_string(index));
      transactions.push_back(transaction);
    }
    log.commit_batch(transactions);
  }
  const auto recovered = wal::WriteAheadLog::recover(path);
  CHECK(recovered.committed_transactions == 32);
  CHECK(recovered.state.size() == 32);
  for (int index = 0; index < 32; ++index) {
    CHECK(recovered.state.at("key-" + std::to_string(index)) ==
          "value-" + std::to_string(index));
  }
  remove_if_present(path);
}

void test_invalid_group_commit_appends_no_commits() {
  const std::string path = temp_path("invalid-group");
  remove_if_present(path);
  {
    wal::WriteAheadLog log(path);
    const auto transaction = log.begin();
    log.put(transaction, "key", "value");
    bool rejected = false;
    try {
      log.commit_batch({transaction, 999});
    } catch (const std::logic_error&) {
      rejected = true;
    }
    CHECK(rejected);
  }
  const auto recovered = wal::WriteAheadLog::recover(path);
  CHECK(recovered.committed_transactions == 0);
  CHECK(recovered.state.empty());
  remove_if_present(path);
}

void test_invalid_transaction_use_is_rejected() {
  const std::string path = temp_path("invalid");
  remove_if_present(path);
  wal::WriteAheadLog log(path);
  bool put_rejected = false;
  bool commit_rejected = false;
  try {
    log.put(999, "key", "value");
  } catch (const std::logic_error&) {
    put_rejected = true;
  }
  try {
    log.commit(999);
  } catch (const std::logic_error&) {
    commit_rejected = true;
  }
  CHECK(put_rejected);
  CHECK(commit_rejected);
  remove_if_present(path);
}

}  // namespace

int main() {
  test_crc32_reference_vector();
  test_committed_transaction_recovers_atomically();
  test_uncommitted_transaction_is_invisible();
  test_delete_replays();
  test_transaction_ids_continue_after_restart();
  test_truncated_tail_stops_before_partial_transaction();
  test_checksum_corruption_is_not_replayed();
  test_every_truncation_of_a_transaction_is_atomic();
  test_group_commit_recovers_every_transaction();
  test_invalid_group_commit_appends_no_commits();
  test_invalid_transaction_use_is_rejected();

  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
