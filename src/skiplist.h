#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <random>

namespace strata {

template <typename Key, typename Comparator>
class SkipList {
 private:
  struct Node;

 public:
  explicit SkipList(Comparator cmp)
      : cmp_(cmp),
        head_(NewNode(Key(), kMaxHeight)),
        max_height_(1),
        rng_(0xdeadbeef),
        dist_(0, 3) {
    for (int i = 0; i < kMaxHeight; ++i) {
      head_->SetNext(i, nullptr);
    }
  }

  ~SkipList() {
    Node* curr = head_;
    while (curr != nullptr) {
      Node* next = curr->Next(0);
      curr->~Node();
      delete[] reinterpret_cast<char*>(curr);
      curr = next;
    }
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);

    assert(x == nullptr || (cmp_(key, x->key) != 0));

    int height = RandomHeight();
    if (height > GetMaxHeight()) {
      for (int i = GetMaxHeight(); i < height; ++i) {
        prev[i] = head_;
      }
      max_height_.store(height, std::memory_order_relaxed);
    }

    x = NewNode(key, height);
    for (int i = 0; i < height; ++i) {
      x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
      prev[i]->SetNext(i, x);
    }
  }

  bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key, nullptr);
    return (x != nullptr && (cmp_(key, x->key) == 0));
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }

    const Key& key() const {
      assert(Valid());
      return node_->key;
    }

    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }

    void Prev() {
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

    void Seek(const Key& target) {
      node_ = list_->FindGreaterOrEqual(target, nullptr);
    }

    void SeekToFirst() {
      node_ = list_->head_->Next(0);
    }

    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  enum { kMaxHeight = 16 };

  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;

    Node* Next(int n) {
      assert(n >= 0);
      return next_[n].load(std::memory_order_acquire);
    }

    void SetNext(int n, Node* x) {
      assert(n >= 0);
      next_[n].store(x, std::memory_order_release);
    }

    Node* NoBarrier_Next(int n) {
      assert(n >= 0);
      return next_[n].load(std::memory_order_relaxed);
    }

    void NoBarrier_SetNext(int n, Node* x) {
      assert(n >= 0);
      next_[n].store(x, std::memory_order_relaxed);
    }

   private:
    std::atomic<Node*> next_[1];
  };

  int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Node* NewNode(const Key& key, int height) {
    size_t alloc_size = sizeof(Node) + sizeof(std::atomic<Node*>) * (height > 0 ? height - 1 : 0);
    char* mem = new char[alloc_size];
    Node* node = new (mem) Node(key);
    for (int i = 0; i < height; ++i) {
      node->NoBarrier_SetNext(i, nullptr);
    }
    return node;
  }

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && dist_(rng_) == 0) {
      height++;
    }
    return height;
  }

  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (n != head_) && (cmp_(n->key, key) < 0);
  }

  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (KeyIsAfterNode(key, next)) {
        x = next;
      } else {
        if (prev != nullptr) {
          prev[level] = x;
        }
        if (level == 0) {
          return next;
        } else {
          level--;
        }
      }
    }
  }

  Node* FindLessThan(const Key& key) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      assert(x == head_ || cmp_(x->key, key) < 0);
      Node* next = x->Next(level);
      if (next == nullptr || cmp_(next->key, key) >= 0) {
        if (level == 0) {
          return x;
        } else {
          level--;
        }
      } else {
        x = next;
      }
    }
  }

  Node* FindLast() const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next == nullptr) {
        if (level == 0) {
          return x;
        } else {
          level--;
        }
      } else {
        x = next;
      }
    }
  }

  Comparator const cmp_;
  Node* const head_;
  std::atomic<int> max_height_;
  mutable std::mt19937 rng_;
  std::uniform_int_distribution<int> dist_;
};

}  // namespace strata
