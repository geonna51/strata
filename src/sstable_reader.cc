#include "sstable_reader.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include "bloom_filter.h"
#include "coding.h"
#include "crc32.h"

namespace strata {

static constexpr uint64_t kTableMagicNumber = 0x5354524154413031ULL;  // "STRATA01"

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(std::shared_ptr<SSTableReader> reader, BlockCache* cache)
      : reader_(std::move(reader)),
        cache_(cache),
        index_iter_(reader_->index_block_->NewIterator()),
        data_iter_(nullptr),
        data_block_(nullptr) {}

  ~TwoLevelIterator() override {
    delete index_iter_;
    delete data_iter_;
  }

  bool Valid() const override {
    return data_iter_ != nullptr && data_iter_->Valid();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToFirst();
    }
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToLast();
    }
    SkipEmptyDataBlocksBackward();
  }

  void Seek(const std::string& target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->Seek(target);
    }
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    assert(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

  std::string Key() const override {
    assert(Valid());
    return data_iter_->Key();
  }

  std::string Value() const override {
    assert(Valid());
    return data_iter_->Value();
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    if (data_iter_ != nullptr && !data_iter_->status().ok()) {
      return data_iter_->status();
    }
    return Status::OK();
  }

  SequenceNumber Seq() const {
    assert(Valid());
    return static_cast<Block::Iter*>(data_iter_)->Seq();
  }

  ValueType Type() const {
    assert(Valid());
    return static_cast<Block::Iter*>(data_iter_)->Type();
  }

 private:
  void InitDataBlock() {
    delete data_iter_;
    data_iter_ = nullptr;
    data_block_ = nullptr;

    if (!index_iter_->Valid()) {
      return;
    }

    Slice handle_slice = index_iter_->Value();
    BlockHandle handle;
    Status s = handle.DecodeFrom(&handle_slice);
    if (!s.ok()) {
      status_ = s;
      return;
    }

    if (cache_ != nullptr) {
      data_block_ = cache_->Lookup(reader_->FileNumber(), handle.offset);
    }

    if (data_block_ == nullptr) {
      s = reader_->ReadBlock(handle, &data_block_);
      if (!s.ok()) {
        status_ = s;
        return;
      }
      if (cache_ != nullptr) {
        cache_->Insert(reader_->FileNumber(), handle.offset, data_block_,
                       data_block_->size());
      }
    }

    data_iter_ = data_block_->NewIterator();
  }

  void SkipEmptyDataBlocksForward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        delete data_iter_;
        data_iter_ = nullptr;
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToFirst();
      }
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        delete data_iter_;
        data_iter_ = nullptr;
        return;
      }
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToLast();
      }
    }
  }

  std::shared_ptr<SSTableReader> reader_;
  BlockCache* cache_;
  Block::Iter* index_iter_;
  Iterator* data_iter_;
  std::shared_ptr<Block> data_block_;
  Status status_;
};

SSTableReader::SSTableReader(const Options& options, const std::string& filename,
                             uint64_t file_number, uint64_t file_size, int fd)
    : options_(options),
      filename_(filename),
      file_number_(file_number),
      file_size_(file_size),
      fd_(fd) {}

SSTableReader::~SSTableReader() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Status SSTableReader::Open(const Options& options, const std::string& filename,
                          uint64_t file_number, uint64_t file_size,
                          std::shared_ptr<SSTableReader>* reader) {
  if (file_size < 48) {
    return Status::Corruption("File too short for SSTable: " + filename);
  }

  int fd = ::open(filename.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("Failed to open SSTable: " + filename + " (" +
                           strerror(errno) + ")");
  }

  char footer[48];
  ssize_t n = ::pread(fd, footer, 48, file_size - 48);
  if (n != 48) {
    ::close(fd);
    return Status::IOError("Failed to read SSTable footer: " + filename);
  }

  uint64_t magic = DecodeFixed64(footer + 40);
  if (magic != kTableMagicNumber) {
    ::close(fd);
    return Status::Corruption("Bad SSTable magic number: " + filename);
  }

  Slice filter_handle_slice(footer, 16);
  BlockHandle filter_handle;
  Status s = filter_handle.DecodeFrom(&filter_handle_slice);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }

  Slice index_handle_slice(footer + 16, 16);
  BlockHandle index_handle;
  s = index_handle.DecodeFrom(&index_handle_slice);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }

  auto r = std::shared_ptr<SSTableReader>(
      new SSTableReader(options, filename, file_number, file_size, fd));

  // Read index block
  std::shared_ptr<Block> idx_blk;
  s = r->ReadBlock(index_handle, &idx_blk);
  if (!s.ok()) {
    return s;
  }
  r->index_block_ = idx_blk;

  // Read filter block
  if (filter_handle.size > 0) {
    std::string buf;
    buf.resize(filter_handle.size + 4);
    ssize_t fn = ::pread(fd, &buf[0], filter_handle.size + 4, filter_handle.offset);
    if (fn == static_cast<ssize_t>(filter_handle.size + 4)) {
      uint32_t expected_crc = DecodeFixed32(&buf[filter_handle.size]);
      uint32_t actual_crc = Crc32(buf.data(), filter_handle.size);
      if (actual_crc == expected_crc) {
        buf.resize(filter_handle.size);
        r->filter_data_ = std::move(buf);
      }
    }
  }

  *reader = std::move(r);
  return Status::OK();
}

Status SSTableReader::ReadBlock(const BlockHandle& handle,
                               std::shared_ptr<Block>* result) {
  std::string buf;
  buf.resize(handle.size + 4);
  ssize_t n = ::pread(fd_, &buf[0], handle.size + 4, handle.offset);
  if (n != static_cast<ssize_t>(handle.size + 4)) {
    return Status::IOError("Failed to read block from file: " + filename_);
  }

  uint32_t expected_crc = DecodeFixed32(&buf[handle.size]);
  uint32_t actual_crc = Crc32(buf.data(), handle.size);
  if (actual_crc != expected_crc) {
    return Status::Corruption("Block checksum mismatch in " + filename_);
  }

  buf.resize(handle.size);
  *result = std::make_shared<Block>(std::move(buf));
  return Status::OK();
}

bool SSTableReader::Get(const Slice& user_key, SequenceNumber seq,
                        std::string* value, Status* s, BlockCache* cache) {
  // Check Bloom Filter first to avoid unnecessary disk I/O
  if (!filter_data_.empty() && !BloomFilter::KeyMayMatch(user_key, filter_data_)) {
    return false;
  }

  std::unique_ptr<Block::Iter> index_iter(index_block_->NewIterator());
  index_iter->Seek(user_key.ToString());
  if (!index_iter->Valid()) {
    return false;
  }

  Slice handle_slice = index_iter->Value();
  BlockHandle handle;
  Status decode_s = handle.DecodeFrom(&handle_slice);
  if (!decode_s.ok()) {
    *s = decode_s;
    return true;
  }

  std::shared_ptr<Block> data_block;
  if (cache != nullptr) {
    data_block = cache->Lookup(file_number_, handle.offset);
  }

  if (data_block == nullptr) {
    Status read_s = ReadBlock(handle, &data_block);
    if (!read_s.ok()) {
      *s = read_s;
      return true;
    }
    if (cache != nullptr) {
      cache->Insert(file_number_, handle.offset, data_block, data_block->size());
    }
  }

  std::unique_ptr<Block::Iter> data_iter(data_block->NewIterator());
  data_iter->Seek(user_key.ToString());

  while (data_iter->Valid() && data_iter->Key() == user_key.ToString()) {
    if (data_iter->Seq() <= seq) {
      if (data_iter->Type() == kTypeValue) {
        if (value != nullptr) {
          *value = data_iter->Value();
        }
        *s = Status::OK();
        return true;
      } else if (data_iter->Type() == kTypeDeletion) {
        *s = Status::NotFound();
        return true;
      }
    }
    data_iter->Next();
  }

  return false;
}

Iterator* SSTableReader::NewIterator(BlockCache* cache) {
  return new TwoLevelIterator(
      std::shared_ptr<SSTableReader>(this, [](SSTableReader*) {}), cache);
}

}  // namespace strata
