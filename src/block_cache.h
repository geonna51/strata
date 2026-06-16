#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "block.h"

namespace strata {

class BlockCache {
 public:
  explicit BlockCache(size_t capacity_bytes);
  ~BlockCache();

  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // Look up block in cache. Returns nullptr on cache miss.
  std::shared_ptr<Block> Lookup(uint64_t file_number, uint64_t offset);

  // Insert a block into the cache with a charge (size in bytes).
  void Insert(uint64_t file_number, uint64_t offset,
              std::shared_ptr<Block> block, size_t charge);

  // Erase a specific cached block.
  void Erase(uint64_t file_number, uint64_t offset);

  // Clear all entries from cache.
  void Clear();

  size_t TotalCharge() const;
  size_t Capacity() const { return capacity_bytes_; }

 private:
  struct CacheKey {
    uint64_t file_number;
    uint64_t offset;

    bool operator==(const CacheKey& other) const {
      return file_number == other.file_number && offset == other.offset;
    }
  };

  struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const {
      return std::hash<uint64_t>()(k.file_number) ^
             (std::hash<uint64_t>()(k.offset) << 1);
    }
  };

  struct CacheEntry {
    CacheKey key;
    std::shared_ptr<Block> block;
    size_t charge;
  };

  mutable std::mutex mutex_;
  size_t capacity_bytes_;
  size_t total_charge_;

  using EntryList = std::list<CacheEntry>;
  EntryList lru_list_;
  std::unordered_map<CacheKey, EntryList::iterator, CacheKeyHash> table_;
};

}  // namespace strata
