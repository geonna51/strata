#include "block_cache.h"

namespace strata {

BlockCache::BlockCache(size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes), total_charge_(0) {}

BlockCache::~BlockCache() {
  Clear();
}

std::shared_ptr<Block> BlockCache::Lookup(uint64_t file_number, uint64_t offset) {
  std::lock_guard<std::mutex> lock(mutex_);
  CacheKey key{file_number, offset};
  auto it = table_.find(key);
  if (it == table_.end()) {
    return nullptr;
  }
  // Move to front (most recently used)
  lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
  return it->second->block;
}

void BlockCache::Insert(uint64_t file_number, uint64_t offset,
                        std::shared_ptr<Block> block, size_t charge) {
  std::lock_guard<std::mutex> lock(mutex_);
  CacheKey key{file_number, offset};

  auto it = table_.find(key);
  if (it != table_.end()) {
    total_charge_ -= it->second->charge;
    lru_list_.erase(it->second);
    table_.erase(it);
  }

  // Evict old entries until charge fits
  while (total_charge_ + charge > capacity_bytes_ && !lru_list_.empty()) {
    auto last = --lru_list_.end();
    total_charge_ -= last->charge;
    table_.erase(last->key);
    lru_list_.pop_back();
  }

  lru_list_.push_front(CacheEntry{key, std::move(block), charge});
  table_[key] = lru_list_.begin();
  total_charge_ += charge;
}

void BlockCache::Erase(uint64_t file_number, uint64_t offset) {
  std::lock_guard<std::mutex> lock(mutex_);
  CacheKey key{file_number, offset};
  auto it = table_.find(key);
  if (it != table_.end()) {
    total_charge_ -= it->second->charge;
    lru_list_.erase(it->second);
    table_.erase(it);
  }
}

void BlockCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  lru_list_.clear();
  table_.clear();
  total_charge_ = 0;
}

size_t BlockCache::TotalCharge() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_charge_;
}

}  // namespace strata
