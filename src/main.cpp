#include "wal/wal.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void usage() {
  std::cerr
      << "usage:\n"
      << "  wal_tool dump <wal>\n"
      << "  wal_tool workload <wal> <oracle> [sleep-us]\n"
      << "  wal_tool benchmark <wal> <transactions> <ops-per-transaction>\n";
}

void repair_oracle_tail(const std::string& path) {
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    throw std::runtime_error("failed to open oracle for repair");
  }

  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    ::close(fd);
    throw std::runtime_error("failed to inspect oracle");
  }
  const std::size_t size = static_cast<std::size_t>(info.st_size);
  if (size == 0) {
    ::close(fd);
    return;
  }

  std::vector<char> bytes(size);
  const ssize_t count = ::pread(fd, bytes.data(), bytes.size(), 0);
  if (count != static_cast<ssize_t>(bytes.size())) {
    ::close(fd);
    throw std::runtime_error("failed to read oracle");
  }

  std::size_t valid_size = bytes.size();
  if (bytes.back() != '\n') {
    const auto newline = std::find(bytes.rbegin(), bytes.rend(), '\n');
    valid_size = newline == bytes.rend()
                     ? 0
                     : bytes.size() -
                           static_cast<std::size_t>(
                               std::distance(bytes.rbegin(), newline));
  }
  if (valid_size != bytes.size() &&
      ::ftruncate(fd, static_cast<off_t>(valid_size)) != 0) {
    ::close(fd);
    throw std::runtime_error("failed to truncate oracle tail");
  }
  if (valid_size != bytes.size() && ::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("failed to sync repaired oracle");
  }
  ::close(fd);
}

void oracle_append(const std::string& path, std::uint64_t transaction_id,
                   const std::string& key, const std::string& value) {
  const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd < 0) {
    throw std::runtime_error("failed to open oracle");
  }
  const std::string line = std::to_string(transaction_id) + "\t" + key + "\t" +
                           value + "\n";
  const ssize_t written = ::write(fd, line.data(), line.size());
  if (written != static_cast<ssize_t>(line.size())) {
    ::close(fd);
    throw std::runtime_error("failed to append complete oracle record");
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("failed to sync oracle");
  }
  ::close(fd);
}

int dump(const std::string& path) {
  const wal::RecoveryResult result = wal::WriteAheadLog::recover(path, false);
  std::cout << "SUMMARY\t" << result.valid_bytes << "\t" << result.file_bytes
            << "\t" << result.valid_records << "\t"
            << result.committed_transactions << "\t"
            << result.discarded_transactions << "\t"
            << (result.tail_damaged ? 1 : 0) << "\n";
  for (const auto& [key, value] : result.state) {
    std::cout << "KV\t" << key << "\t" << value << "\n";
  }
  return 0;
}

int workload(const std::string& wal_path, const std::string& oracle_path,
             int sleep_microseconds) {
  repair_oracle_tail(oracle_path);
  wal::WriteAheadLog log(wal_path, true);
  for (;;) {
    const std::uint64_t transaction_id = log.begin();
    const std::string key = "tx-" + std::to_string(transaction_id);
    const std::string value = "value-" + std::to_string(transaction_id);
    log.put(transaction_id, key, value);
    if (sleep_microseconds > 0) {
      ::usleep(static_cast<useconds_t>(sleep_microseconds));
    }
    log.commit(transaction_id);
    oracle_append(oracle_path, transaction_id, key, value);
  }
}

int benchmark(const std::string& path, std::size_t transaction_count,
              std::size_t operations_per_transaction) {
  const auto started = std::chrono::steady_clock::now();
  {
    wal::WriteAheadLog log(path, true);
    for (std::size_t tx_index = 0; tx_index < transaction_count; ++tx_index) {
      const std::uint64_t transaction_id = log.begin();
      for (std::size_t op = 0; op < operations_per_transaction; ++op) {
        log.put(transaction_id,
                "key-" + std::to_string(tx_index) + "-" + std::to_string(op),
                "value-" + std::to_string(tx_index));
      }
      log.commit(transaction_id);
    }
  }
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started);
  const double transactions_per_second =
      static_cast<double>(transaction_count) / elapsed.count();
  const wal::RecoveryResult recovered =
      wal::WriteAheadLog::recover(path, false);
  std::cout << "transactions=" << transaction_count
            << " ops_per_tx=" << operations_per_transaction
            << " seconds=" << elapsed.count()
            << " tx_per_second=" << transactions_per_second
            << " recovered_keys=" << recovered.state.size() << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "dump") {
      return dump(argv[2]);
    }
    if ((argc == 4 || argc == 5) &&
        std::string(argv[1]) == "workload") {
      const int sleep_us = argc == 5 ? std::stoi(argv[4]) : 0;
      return workload(argv[2], argv[3], sleep_us);
    }
    if (argc == 5 && std::string(argv[1]) == "benchmark") {
      return benchmark(argv[2], std::stoull(argv[3]),
                       std::stoull(argv[4]));
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  usage();
  return 2;
}
