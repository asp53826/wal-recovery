#include "wal/wal.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
  test_invalid_transaction_use_is_rejected();

  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}

