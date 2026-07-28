#include "wal/wal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace wal {
namespace {

constexpr std::uint32_t kMagic = 0x314C4157U;  // "WAL1" little-endian.
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kChecksumSize = 4;
constexpr std::uint32_t kMaxKeyBytes = 1U << 20;
constexpr std::uint32_t kMaxValueBytes = 16U << 20;

[[noreturn]] void throw_system_error(const std::string& operation) {
  throw std::runtime_error(operation + ": " + std::strerror(errno));
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(*data++) << shift;
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(*data++) << shift;
  }
  return value;
}

void write_all(int fd, const std::uint8_t* data, std::size_t size) {
  const char* split = std::getenv("WAL_SPLIT_WRITES");
  const bool split_writes = split != nullptr && std::string(split) == "1";
  const std::size_t chunk_size = split_writes ? 7 : size;

  std::size_t written = 0;
  while (written < size) {
    const std::size_t requested = std::min(chunk_size, size - written);
    const ssize_t result = ::write(fd, data + written, requested);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_system_error("write");
    }
    if (result == 0) {
      throw std::runtime_error("write returned zero bytes");
    }
    written += static_cast<std::size_t>(result);
    if (split_writes) {
      ::usleep(150);
    }
  }
}

bool read_exact(int fd, std::uint64_t offset, std::uint8_t* data,
                std::size_t size) {
  std::size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t result =
        ::pread(fd, data + read_bytes, size - read_bytes,
                static_cast<off_t>(offset + read_bytes));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_system_error("pread");
    }
    if (result == 0) {
      return false;
    }
    read_bytes += static_cast<std::size_t>(result);
  }
  return true;
}

bool valid_record_type(std::uint8_t raw) {
  return raw >= static_cast<std::uint8_t>(RecordType::Begin) &&
         raw <= static_cast<std::uint8_t>(RecordType::Commit);
}

void apply_operations(
    std::unordered_map<std::string, std::string>& state,
    const std::vector<Operation>& operations) {
  for (const Operation& operation : operations) {
    if (operation.type == RecordType::Put) {
      state[operation.key] = operation.value;
    } else if (operation.type == RecordType::Delete) {
      state.erase(operation.key);
    }
  }
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

WriteAheadLog::WriteAheadLog(std::string path, bool repair_invalid_tail)
    : path_(std::move(path)) {
  const RecoveryResult recovered = recover(path_, repair_invalid_tail);
  next_transaction_id_ = recovered.max_transaction_id + 1;
  fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (fd_ < 0) {
    throw_system_error("open " + path_);
  }
}

WriteAheadLog::~WriteAheadLog() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

std::uint64_t WriteAheadLog::begin() {
  const std::uint64_t transaction_id = next_transaction_id_++;
  append(RecordType::Begin, transaction_id, {}, {});
  active_transactions_.insert(transaction_id);
  return transaction_id;
}

void WriteAheadLog::put(std::uint64_t transaction_id, const std::string& key,
                        const std::string& value) {
  require_active(transaction_id);
  if (key.empty()) {
    throw std::invalid_argument("keys must not be empty");
  }
  append(RecordType::Put, transaction_id, key, value);
}

void WriteAheadLog::erase(std::uint64_t transaction_id,
                          const std::string& key) {
  require_active(transaction_id);
  if (key.empty()) {
    throw std::invalid_argument("keys must not be empty");
  }
  append(RecordType::Delete, transaction_id, key, {});
}

void WriteAheadLog::commit(std::uint64_t transaction_id) {
  commit_batch({transaction_id});
}

void WriteAheadLog::commit_batch(
    const std::vector<std::uint64_t>& transaction_ids) {
  if (transaction_ids.empty()) {
    throw std::invalid_argument("commit batch must not be empty");
  }
  std::unordered_set<std::uint64_t> unique;
  for (const std::uint64_t transaction_id : transaction_ids) {
    require_active(transaction_id);
    if (!unique.insert(transaction_id).second) {
      throw std::invalid_argument("commit batch contains duplicate transaction");
    }
  }
  for (const std::uint64_t transaction_id : transaction_ids) {
    append(RecordType::Commit, transaction_id, {}, {});
  }
  if (::fsync(fd_) != 0) {
    throw_system_error("fsync");
  }
  for (const std::uint64_t transaction_id : transaction_ids) {
    active_transactions_.erase(transaction_id);
  }
}

void WriteAheadLog::require_active(std::uint64_t transaction_id) const {
  if (active_transactions_.find(transaction_id) ==
      active_transactions_.end()) {
    throw std::logic_error("transaction is not active");
  }
}

void WriteAheadLog::append(RecordType type, std::uint64_t transaction_id,
                           const std::string& key,
                           const std::string& value) {
  if (key.size() > kMaxKeyBytes || value.size() > kMaxValueBytes) {
    throw std::length_error("WAL record exceeds configured size limit");
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(kHeaderSize + key.size() + value.size() + kChecksumSize);
  append_u32(bytes, kMagic);
  bytes.push_back(kVersion);
  bytes.push_back(static_cast<std::uint8_t>(type));
  append_u16(bytes, 0);
  append_u64(bytes, transaction_id);
  append_u32(bytes, static_cast<std::uint32_t>(key.size()));
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), key.begin(), key.end());
  bytes.insert(bytes.end(), value.begin(), value.end());
  append_u32(bytes, crc32(bytes.data(), bytes.size()));

  write_all(fd_, bytes.data(), bytes.size());
}

RecoveryResult WriteAheadLog::recover(const std::string& path,
                                      bool repair_invalid_tail) {
  RecoveryResult result;
  const int flags = repair_invalid_tail ? O_RDWR : O_RDONLY;
  const int fd = ::open(path.c_str(), flags);
  if (fd < 0) {
    if (errno == ENOENT) {
      return result;
    }
    throw_system_error("open " + path);
  }

  try {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      throw_system_error("fstat");
    }
    result.file_bytes = static_cast<std::uint64_t>(info.st_size);

    std::unordered_map<std::uint64_t, std::vector<Operation>> pending;
    std::uint64_t offset = 0;
    bool invalid = false;

    while (offset < result.file_bytes) {
      const std::uint64_t remaining = result.file_bytes - offset;
      if (remaining < kHeaderSize) {
        invalid = true;
        break;
      }

      std::vector<std::uint8_t> header(kHeaderSize);
      if (!read_exact(fd, offset, header.data(), header.size())) {
        invalid = true;
        break;
      }

      const std::uint32_t magic = read_u32(header.data());
      const std::uint8_t version = header[4];
      const std::uint8_t raw_type = header[5];
      const std::uint16_t reserved = read_u16(header.data() + 6);
      const std::uint64_t transaction_id = read_u64(header.data() + 8);
      const std::uint32_t key_length = read_u32(header.data() + 16);
      const std::uint32_t value_length = read_u32(header.data() + 20);

      if (magic != kMagic || version != kVersion || reserved != 0 ||
          !valid_record_type(raw_type) || transaction_id == 0 ||
          key_length > kMaxKeyBytes || value_length > kMaxValueBytes) {
        invalid = true;
        break;
      }

      const std::uint64_t record_size =
          kHeaderSize + static_cast<std::uint64_t>(key_length) +
          static_cast<std::uint64_t>(value_length) + kChecksumSize;
      if (record_size > remaining ||
          record_size > std::numeric_limits<std::size_t>::max()) {
        invalid = true;
        break;
      }

      std::vector<std::uint8_t> record(static_cast<std::size_t>(record_size));
      if (!read_exact(fd, offset, record.data(), record.size())) {
        invalid = true;
        break;
      }

      const std::uint32_t stored_crc =
          read_u32(record.data() + record.size() - kChecksumSize);
      const std::uint32_t calculated_crc =
          crc32(record.data(), record.size() - kChecksumSize);
      if (stored_crc != calculated_crc) {
        invalid = true;
        break;
      }

      const RecordType type = static_cast<RecordType>(raw_type);
      const auto pending_it = pending.find(transaction_id);
      const char* payload =
          reinterpret_cast<const char*>(record.data() + kHeaderSize);
      const std::string key(payload, key_length);
      const std::string value(payload + key_length, value_length);

      bool valid_sequence = true;
      switch (type) {
        case RecordType::Begin:
          valid_sequence = pending_it == pending.end() && key.empty() &&
                           value.empty();
          if (valid_sequence) {
            pending.emplace(transaction_id, std::vector<Operation>{});
          }
          break;
        case RecordType::Put:
          valid_sequence = pending_it != pending.end() && !key.empty();
          if (valid_sequence) {
            pending_it->second.push_back({type, key, value});
          }
          break;
        case RecordType::Delete:
          valid_sequence =
              pending_it != pending.end() && !key.empty() && value.empty();
          if (valid_sequence) {
            pending_it->second.push_back({type, key, {}});
          }
          break;
        case RecordType::Commit:
          valid_sequence = pending_it != pending.end() && key.empty() &&
                           value.empty();
          if (valid_sequence) {
            apply_operations(result.state, pending_it->second);
            pending.erase(pending_it);
            ++result.committed_transactions;
          }
          break;
      }

      if (!valid_sequence) {
        invalid = true;
        break;
      }

      result.max_transaction_id =
          std::max(result.max_transaction_id, transaction_id);
      offset += record_size;
      result.valid_bytes = offset;
      ++result.valid_records;
    }

    result.discarded_transactions = pending.size();
    result.tail_damaged = invalid;

    if (invalid && repair_invalid_tail) {
      if (::ftruncate(fd, static_cast<off_t>(result.valid_bytes)) != 0) {
        throw_system_error("ftruncate");
      }
      if (::fsync(fd) != 0) {
        throw_system_error("fsync after ftruncate");
      }
      result.file_bytes = result.valid_bytes;
    }
  } catch (...) {
    ::close(fd);
    throw;
  }

  ::close(fd);
  return result;
}

}  // namespace wal
