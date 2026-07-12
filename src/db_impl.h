#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "block_cache.h"
#include "format.h"
#include "memtable.h"
#include "strata/db.h"
#include "strata/options.h"
#include "strata/status.h"
#include "version_set.h"
#include "wal.h"

namespace strata {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  Status Put(const std::string& key, const std::string& value) override;
  Status Get(const std::string& key, std::string* value) override;
  Status Delete(const std::string& key) override;
  Iterator* NewIterator() override;

  // Flush active memtable to SSTable. Useful for testing and manual sync.
  Status FlushMemTable();

  // Trigger synchronous compaction step. Useful for testing.
  Status CompactRange();

 private:
  friend class DB;

  Status Recover();
  Status MakeRoomForWrite(std::unique_lock<std::mutex>* lock);
  void BackgroundThread();
  void BackgroundCompaction();
  Status WriteLevel0Table(Memtable* mem, VersionEdit* edit);
  void RemoveObsoleteFiles();

  Options options_;
  std::string dbname_;
  int lock_fd_;

  std::unique_ptr<BlockCache> block_cache_;
  std::unique_ptr<VersionSet> versions_;

  std::mutex mutex_;
  std::condition_variable bg_cv_;
  std::condition_variable write_cv_;

  std::shared_ptr<Memtable> mem_;
  std::shared_ptr<Memtable> imm_;
  std::unique_ptr<WalWriter> wal_writer_;
  uint64_t log_file_number_;

  bool shutting_down_;
  std::thread background_thread_;
};

}  // namespace strata
