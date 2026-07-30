#include "db_impl.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include "compaction.h"
#include "db_iterator.h"
#include "env.h"
#include "filename.h"
#include "merging_iterator.h"
#include "sstable_builder.h"

namespace strata {

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;
  ::mkdir(name.c_str(), 0755);

  auto impl = std::make_unique<DBImpl>(options, name);
  Status s = impl->Recover();
  if (!s.ok()) {
    return s;
  }

  *dbptr = impl.release();
  return Status::OK();
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : options_(options),
      dbname_(dbname),
      lock_fd_(-1),
      block_cache_(std::make_unique<BlockCache>(options.block_cache_capacity_bytes)),
      versions_(std::make_unique<VersionSet>(dbname_, &options_, block_cache_.get())),
      log_file_number_(0),
      shutting_down_(false) {}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    bg_cv_.notify_all();
  }

  if (background_thread_.joinable()) {
    background_thread_.join();
  }

  if (wal_writer_) {
    wal_writer_->Close();
  }

  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
  }
}

Status DBImpl::Recover() {
  // Lock database directory
  std::string lock_file = LockFileName(dbname_);
  lock_fd_ = ::open(lock_file.c_str(), O_RDWR | O_CREAT, 0644);
  if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    return Status::IOError("Database is locked by another process: " + dbname_);
  }

  bool is_new_db = false;
  std::string current_file = CurrentFileName(dbname_);
  if (::access(current_file.c_str(), F_OK) != 0) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument("Database does not exist and create_if_missing is false");
    }
    is_new_db = true;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument("Database exists and error_if_exists is true");
  }

  if (is_new_db) {
    VersionEdit edit;
    edit.SetComparatorName("byte_comparator");
    edit.SetLogNumber(0);
    edit.SetNextFile(2);
    edit.SetLastSequence(0);
    Status s = versions_->LogAndApply(&edit);
    if (!s.ok()) return s;
  } else {
    Status s = versions_->Recover();
    if (!s.ok()) return s;
  }

  // Scan for existing WAL logs to recover uncommitted writes
  DIR* dir = ::opendir(dbname_.c_str());
  if (dir != nullptr) {
    struct dirent* entry;
    std::vector<uint64_t> log_files;
    while ((entry = ::readdir(dir)) != nullptr) {
      std::string fname = entry->d_name;
      if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".log") {
        uint64_t num = std::stoull(fname.substr(0, fname.size() - 4));
        if (num >= versions_->LogNumber()) {
          log_files.push_back(num);
        }
      }
    }
    ::closedir(dir);
    std::sort(log_files.begin(), log_files.end());

    for (uint64_t log_num : log_files) {
      std::string log_path = LogFileName(dbname_, log_num);
      WalReader reader(log_path);
      Memtable recovery_mem;

      SequenceNumber seq = 0;
      ValueType type = kTypeValue;
      std::string key, val;
      SequenceNumber max_seq = versions_->LastSequence();

      while (reader.ReadRecord(&seq, &type, &key, &val, true).ok()) {
        recovery_mem.Add(seq, type, key, val);
        if (seq > max_seq) max_seq = seq;
      }

      versions_->SetLastSequence(max_seq);

      if (recovery_mem.Count() > 0) {
        VersionEdit edit;
        Status s = WriteLevel0Table(&recovery_mem, &edit);
        if (!s.ok()) return s;
        edit.SetLogNumber(log_num);
        s = versions_->LogAndApply(&edit);
        if (!s.ok()) return s;
      }
      ::unlink(log_path.c_str());
    }
  }

  // Allocate new WAL log file and memtable
  log_file_number_ = versions_->NewFileNumber();
  wal_writer_ = std::make_unique<WalWriter>(LogFileName(dbname_, log_file_number_));
  VersionEdit edit;
  edit.SetLogNumber(log_file_number_);
  Status s = versions_->LogAndApply(&edit);
  if (!s.ok()) return s;

  mem_ = std::make_shared<Memtable>();

  // Launch background compaction thread
  background_thread_ = std::thread(&DBImpl::BackgroundThread, this);

  return Status::OK();
}

Status DBImpl::WriteLevel0Table(Memtable* mem, VersionEdit* edit) {
  uint64_t file_num = versions_->NewFileNumber();
  std::string sst_path = TableFileName(dbname_, file_num);

  SSTableBuilder builder(options_, sst_path);
  std::unique_ptr<Memtable::MemTableIterator> it(mem->NewIterator());
  it->SeekToFirst();

  while (it->Valid()) {
    builder.Add(it->Seq(), it->Type(), it->Key(), it->Value());
    it->Next();
  }

  Status s = builder.Finish();
  if (!s.ok()) {
    return s;
  }

  std::shared_ptr<SSTableReader> reader;
  Status s_open = SSTableReader::Open(options_, sst_path, file_num, builder.FileSize(), &reader);
  if (!s_open.ok()) {
    return s_open;
  }

  edit->AddFile(0, file_num, builder.FileSize(), builder.SmallestKey(),
                builder.LargestKey(), std::move(reader));
  SyncDirectory(dbname_);
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>* lock) {
  while (true) {
    if (mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      write_cv_.wait(*lock);
    } else {
      imm_ = mem_;
      mem_ = std::make_shared<Memtable>();
      log_file_number_ = versions_->NewFileNumber();
      wal_writer_ = std::make_unique<WalWriter>(LogFileName(dbname_, log_file_number_));
      bg_cv_.notify_one();
      return Status::OK();
    }
  }
}

Status DBImpl::Put(const std::string& key, const std::string& value) {
  std::unique_lock<std::mutex> lock(mutex_);
  Status s = MakeRoomForWrite(&lock);
  if (!s.ok()) return s;

  SequenceNumber seq = versions_->LastSequence() + 1;
  s = wal_writer_->AppendRecord(seq, kTypeValue, key, value, options_.sync);
  if (!s.ok()) return s;

  mem_->Add(seq, kTypeValue, key, value);
  versions_->SetLastSequence(seq);

  return Status::OK();
}

Status DBImpl::Delete(const std::string& key) {
  std::unique_lock<std::mutex> lock(mutex_);
  Status s = MakeRoomForWrite(&lock);
  if (!s.ok()) return s;

  SequenceNumber seq = versions_->LastSequence() + 1;
  s = wal_writer_->AppendRecord(seq, kTypeDeletion, key, "", options_.sync);
  if (!s.ok()) return s;

  mem_->Add(seq, kTypeDeletion, key, "");
  versions_->SetLastSequence(seq);

  return Status::OK();
}

Status DBImpl::Get(const std::string& key, std::string* value) {
  std::shared_ptr<Memtable> mem;
  std::shared_ptr<Memtable> imm;
  std::shared_ptr<Version> current_v;
  SequenceNumber seq = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    mem = mem_;
    imm = imm_;
    current_v = versions_->current();
    seq = versions_->LastSequence();
  }

  // 1. Check active Memtable
  Status s;
  if (mem->Get(key, seq, value, &s)) {
    return s;
  }

  // 2. Check immutable Memtable
  if (imm && imm->Get(key, seq, value, &s)) {
    return s;
  }

  // 3. Check SSTables (Level 0 through Level 6)
  if (current_v->Get(options_, key, seq, value, &s, block_cache_.get())) {
    return s;
  }

  return Status::NotFound();
}

Iterator* DBImpl::NewIterator() {
  std::shared_ptr<Memtable> mem;
  std::shared_ptr<Memtable> imm;
  std::shared_ptr<Version> current_v;
  SequenceNumber seq = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    mem = mem_;
    imm = imm_;
    current_v = versions_->current();
    seq = versions_->LastSequence();
  }

  std::vector<std::unique_ptr<InternalIterator>> iters;
  iters.emplace_back(mem->NewIterator(mem));
  if (imm) {
    iters.emplace_back(imm->NewIterator(imm));
  }

  std::vector<Iterator*> table_iters;
  current_v->AddIterators(options_, block_cache_.get(), &table_iters);
  for (auto* tit : table_iters) {
    iters.emplace_back(static_cast<InternalIterator*>(tit));
  }

  std::unique_ptr<InternalIterator> merging_iter(
      NewMergingIterator(std::move(iters)));
  return new DBIterator(std::move(merging_iter), seq, current_v, mem, imm);
}

Status DBImpl::FlushMemTable() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (mem_->Count() == 0 && imm_ == nullptr) {
    return Status::OK();
  }

  if (mem_->Count() > 0) {
    imm_ = mem_;
    mem_ = std::make_shared<Memtable>();
    log_file_number_ = versions_->NewFileNumber();
    wal_writer_ = std::make_unique<WalWriter>(LogFileName(dbname_, log_file_number_));
    bg_cv_.notify_one();
  }

  while (imm_ != nullptr) {
    write_cv_.wait(lock);
  }

  return Status::OK();
}

Status DBImpl::CompactRange() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto c = Compaction::PickCompaction(options_, versions_.get());
  if (c != nullptr) {
    Status s = DoCompaction(options_, dbname_, versions_.get(), c.get(),
                            block_cache_.get());
    RemoveObsoleteFiles();
    return s;
  }
  return Status::OK();
}

void DBImpl::BackgroundThread() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!shutting_down_) {
    while (!shutting_down_ && imm_ == nullptr &&
           Compaction::PickCompaction(options_, versions_.get()) == nullptr) {
      bg_cv_.wait(lock);
    }

    if (shutting_down_) {
      break;
    }

    BackgroundCompaction();
  }
}

void DBImpl::BackgroundCompaction() {
  // 1. Minor compaction (Flush immutable memtable)
  if (imm_ != nullptr) {
    std::shared_ptr<Memtable> imm_flush = imm_;
    VersionEdit edit;
    Status s = WriteLevel0Table(imm_flush.get(), &edit);
    if (s.ok()) {
      edit.SetLogNumber(log_file_number_);
      s = versions_->LogAndApply(&edit);
    }
    imm_.reset();
    write_cv_.notify_all();
  }

  // 2. Major compaction
  auto c = Compaction::PickCompaction(options_, versions_.get());
  if (c != nullptr) {
    DoCompaction(options_, dbname_, versions_.get(), c.get(), block_cache_.get());
  }

  // 3. Remove obsolete files
  RemoveObsoleteFiles();
}

void DBImpl::RemoveObsoleteFiles() {
  std::set<uint64_t> live_files;
  versions_->AddLiveFiles(&live_files);

  DIR* dir = ::opendir(dbname_.c_str());
  if (dir == nullptr) return;

  struct dirent* entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    std::string fname = entry->d_name;
    if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".sst") {
      uint64_t num = std::stoull(fname.substr(0, fname.size() - 4));
      if (live_files.count(num) == 0) {
        ::unlink((dbname_ + "/" + fname).c_str());
      }
    }
  }
  ::closedir(dir);
}

}  // namespace strata
