#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wal {

enum class RecordType : std::uint8_t {
  Begin = 1,
  Put = 2,
  Delete = 3,
  Commit = 4,
};

struct Operation {
  RecordType type;
  std::string key;
  std::string value;
};

struct RecoveryResult {
  std::unordered_map<std::string, std::string> state;
  std::uint64_t valid_bytes = 0;
  std::uint64_t file_bytes = 0;
  std::uint64_t max_transaction_id = 0;
  std::size_t valid_records = 0;
  std::size_t committed_transactions = 0;
  std::size_t discarded_transactions = 0;
  bool tail_damaged = false;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

class WriteAheadLog {
 public:
  explicit WriteAheadLog(std::string path, bool repair_invalid_tail = true);
  ~WriteAheadLog();

  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;
  WriteAheadLog(WriteAheadLog&&) = delete;
  WriteAheadLog& operator=(WriteAheadLog&&) = delete;

  std::uint64_t begin();
  void put(std::uint64_t transaction_id, const std::string& key,
           const std::string& value);
  void erase(std::uint64_t transaction_id, const std::string& key);
  void commit(std::uint64_t transaction_id);
  void commit_batch(const std::vector<std::uint64_t>& transaction_ids);

  const std::string& path() const noexcept { return path_; }

  static RecoveryResult recover(const std::string& path,
                                bool repair_invalid_tail = false);

 private:
  void append(RecordType type, std::uint64_t transaction_id,
              const std::string& key, const std::string& value);
  void require_active(std::uint64_t transaction_id) const;

  std::string path_;
  int fd_ = -1;
  std::uint64_t next_transaction_id_ = 1;
  std::unordered_set<std::uint64_t> active_transactions_;
};

}  // namespace wal
