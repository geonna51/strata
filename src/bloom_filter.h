#pragma once

#include <string>
#include <vector>
#include "strata/slice.h"

namespace strata {

class BloomFilter {
 public:
  // Builds a Bloom filter from a collection of keys.
  // Appends the filter bytes (including a 1-byte probe count trailer) to *dst.
  static void CreateFilter(const std::vector<std::string>& keys,
                           int bits_per_key, std::string* dst);

  // Evaluates whether a key may match the given bloom filter.
  // Returns false if key is definitely not present.
  // Returns true if key might be present (with false positive rate ~1% at 10 bits/key).
  static bool KeyMayMatch(const Slice& key, const Slice& filter);
};

}  // namespace strata
