#include "wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include "coding.h"
#include "crc32.h"

namespace strata {

WalWriter::WalWriter(const std::string& filename)
    : filename_(filename), fd_(-1), file_size_(0) {
  fd_ = ::open(filename.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd_ >= 0) {
    struct stat s;
    if (::fstat(fd_, &s) == 0) {
      file_size_ = s.st_size;
    }
  }
}

WalWriter::~WalWriter() {
  Close();
}

Status WalWriter::AppendRecord(SequenceNumber seq, ValueType type,
                               const Slice& key, const Slice& value,
                               bool sync) {
  if (fd_ < 0) {
    return Status::IOError("WAL file is not open: " + filename_);
  }

  // Layout:
  // [type: 1B] [seq: 8B] [klen: 4B] [key] [vlen: 4B] [val]
  std::string payload;
  payload.reserve(1 + 8 + 4 + key.size() + 4 + value.size());
  payload.push_back(static_cast<char>(type));
  PutFixed64(&payload, seq);
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key.data(), key.size());
  PutFixed32(&payload, static_cast<uint32_t>(value.size()));
  payload.append(value.data(), value.size());

  uint32_t crc = Crc32(payload.data(), payload.size());
  uint32_t length = static_cast<uint32_t>(payload.size());

  char header[8];
  EncodeFixed32(header, crc);
  EncodeFixed32(header + 4, length);

  // Write header
  const char* p = header;
  size_t left = 8;
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::IOError("WAL write failed: " + std::string(strerror(errno)));
    }
    p += n;
    left -= n;
    file_size_ += n;
  }

  // Write payload
  p = payload.data();
  left = payload.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::IOError("WAL write failed: " + std::string(strerror(errno)));
    }
    p += n;
    left -= n;
    file_size_ += n;
  }

  if (sync) {
    return Sync();
  }

  return Status::OK();
}

Status WalWriter::Sync() {
  if (fd_ < 0) {
    return Status::IOError("WAL file is not open: " + filename_);
  }
#if defined(__APPLE__)
  if (::fcntl(fd_, F_FULLFSYNC) < 0) {
    if (::fsync(fd_) < 0) {
      return Status::IOError("fsync failed: " + std::string(strerror(errno)));
    }
  }
#else
  if (::fdatasync(fd_) < 0) {
    return Status::IOError("fdatasync failed: " + std::string(strerror(errno)));
  }
#endif
  return Status::OK();
}

Status WalWriter::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  return Status::OK();
}

WalReader::WalReader(const std::string& filename)
    : filename_(filename), fd_(-1) {
  fd_ = ::open(filename.c_str(), O_RDONLY);
}

WalReader::~WalReader() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Status WalReader::ReadRecord(SequenceNumber* seq, ValueType* type,
                             std::string* key, std::string* value,
                             bool allow_partial_eof) {
  if (fd_ < 0) {
    return Status::IOError("WAL file is not open: " + filename_);
  }

  char header[8];
  size_t bytes_read = 0;
  while (bytes_read < 8) {
    ssize_t n = ::read(fd_, header + bytes_read, 8 - bytes_read);
    if (n == 0) {
      // EOF reached
      if (bytes_read == 0) {
        return Status::NotFound("EOF");
      }
      if (allow_partial_eof) {
        return Status::NotFound("Partial record at EOF");
      }
      return Status::Corruption("Truncated WAL header at EOF");
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IOError("WAL read failed: " + std::string(strerror(errno)));
    }
    bytes_read += n;
  }

  uint32_t expected_crc = DecodeFixed32(header);
  uint32_t length = DecodeFixed32(header + 4);

  std::string payload;
  payload.resize(length);

  bytes_read = 0;
  while (bytes_read < length) {
    ssize_t n = ::read(fd_, &payload[bytes_read], length - bytes_read);
    if (n == 0) {
      if (allow_partial_eof) {
        return Status::NotFound("Partial payload at EOF");
      }
      return Status::Corruption("Truncated WAL payload at EOF");
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IOError("WAL read failed: " + std::string(strerror(errno)));
    }
    bytes_read += n;
  }

  uint32_t actual_crc = Crc32(payload.data(), payload.size());
  if (actual_crc != expected_crc) {
    if (allow_partial_eof) {
      return Status::NotFound("Corrupt trailing record at EOF");
    }
    return Status::Corruption("WAL CRC mismatch");
  }

  // Parse payload: [type: 1B] [seq: 8B] [klen: 4B] [key] [vlen: 4B] [val]
  if (payload.size() < 1 + 8 + 4 + 4) {
    return Status::Corruption("WAL payload too small for fields");
  }

  *type = static_cast<ValueType>(payload[0]);
  *seq = DecodeFixed64(payload.data() + 1);
  uint32_t klen = DecodeFixed32(payload.data() + 9);

  if (payload.size() < 1 + 8 + 4 + klen + 4) {
    return Status::Corruption("WAL payload truncated before key/val_len");
  }

  *key = payload.substr(13, klen);

  uint32_t vlen = DecodeFixed32(payload.data() + 13 + klen);
  if (payload.size() < 1 + 8 + 4 + klen + 4 + vlen) {
    return Status::Corruption("WAL payload truncated before value");
  }

  *value = payload.substr(17 + klen, vlen);

  return Status::OK();
}

}  // namespace strata
