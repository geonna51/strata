#include "sstable_builder.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include "bloom_filter.h"
#include "coding.h"
#include "crc32.h"
#include "env.h"

namespace strata {

static constexpr uint64_t kTableMagicNumber = 0x5354524154413031ULL;  // "STRATA01"

SSTableBuilder::SSTableBuilder(const Options& options, const std::string& filename)
    : options_(options),
      filename_(filename),
      fd_(-1),
      offset_(0),
      num_entries_(0),
      data_block_(16),
      index_block_(1),
      status_(Status::OK()),
      closed_(false) {
  fd_ = ::open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd_ < 0) {
    status_ = Status::IOError("Failed to create file: " + filename + " (" +
                             strerror(errno) + ")");
  }
}

SSTableBuilder::~SSTableBuilder() {
  if (!closed_ && fd_ >= 0) {
    ::close(fd_);
  }
}

void SSTableBuilder::Add(SequenceNumber seq, ValueType type, const Slice& key,
                         const Slice& value) {
  if (!status_.ok()) return;

  std::string kstr = key.ToString();
  if (num_entries_ == 0) {
    smallest_key_ = kstr;
  }
  largest_key_ = kstr;

  keys_for_filter_.push_back(kstr);
  last_key_in_block_ = kstr;

  data_block_.Add(seq, type, key, value);
  num_entries_++;

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushBlock();
  }
}

void SSTableBuilder::FlushBlock() {
  if (data_block_.Empty()) return;

  Slice raw = data_block_.Finish();
  BlockHandle handle;
  status_ = WriteRawBlock(raw, &handle);
  if (!status_.ok()) return;

  std::string handle_encoding;
  handle.EncodeTo(&handle_encoding);
  index_block_.Add(0, kTypeValue, last_key_in_block_, handle_encoding);
  data_block_.Reset();
}

Status SSTableBuilder::WriteRawBlock(const Slice& block_contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = block_contents.size();

  // Write block contents
  const char* p = block_contents.data();
  size_t left = block_contents.size();
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::IOError("Failed to write block: " + std::string(strerror(errno)));
    }
    p += n;
    left -= n;
    offset_ += n;
  }

  // Write 4-byte CRC32 trailer
  uint32_t crc = Crc32(block_contents.data(), block_contents.size());
  char trailer[4];
  EncodeFixed32(trailer, crc);
  p = trailer;
  left = 4;
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::IOError("Failed to write block trailer: " + std::string(strerror(errno)));
    }
    p += n;
    left -= n;
    offset_ += n;
  }

  return Status::OK();
}

Status SSTableBuilder::Finish() {
  if (!status_.ok()) {
    if (fd_ >= 0) {
      ::close(fd_);
      closed_ = true;
    }
    return status_;
  }

  FlushBlock();

  // Write Bloom Filter Block
  std::string filter_data;
  BloomFilter::CreateFilter(keys_for_filter_, options_.bloom_bits_per_key, &filter_data);
  BlockHandle filter_handle;
  status_ = WriteRawBlock(Slice(filter_data), &filter_handle);
  if (!status_.ok()) return status_;

  // Write Index Block
  Slice index_raw = index_block_.Finish();
  BlockHandle index_handle;
  status_ = WriteRawBlock(index_raw, &index_handle);
  if (!status_.ok()) return status_;

  // Write Footer (48 bytes)
  // [filter_handle: 16B] [index_handle: 16B] [padding: 8B] [magic: 8B]
  char footer[48];
  memset(footer, 0, sizeof(footer));
  std::string fh_enc, ih_enc;
  filter_handle.EncodeTo(&fh_enc);
  index_handle.EncodeTo(&ih_enc);

  memcpy(footer, fh_enc.data(), 16);
  memcpy(footer + 16, ih_enc.data(), 16);
  // 8 bytes padding left as 0
  EncodeFixed64(footer + 40, kTableMagicNumber);

  const char* p = footer;
  size_t left = sizeof(footer);
  while (left > 0) {
    ssize_t n = ::write(fd_, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::IOError("Failed to write footer: " + std::string(strerror(errno)));
    }
    p += n;
    left -= n;
    offset_ += n;
  }

  status_ = FsyncFile(fd_);
  ::close(fd_);
  closed_ = true;
  return status_;
}

}  // namespace strata
