#include "bloom_filter.h"
#include <cmath>
#include "coding.h"

namespace strata {

static uint32_t MurmurHash3(const char* data, size_t len, uint32_t seed) {
  uint32_t h = seed;
  const uint32_t c1 = 0xcc9e2d51;
  const uint32_t c2 = 0x1b873593;
  const size_t nblocks = len / 4;

  for (size_t i = 0; i < nblocks; ++i) {
    uint32_t k = DecodeFixed32(data + i * 4);
    k *= c1;
    k = (k << 15) | (k >> 17);
    k *= c2;

    h ^= k;
    h = (h << 13) | (h >> 19);
    h = h * 5 + 0xe6546b64;
  }

  const uint8_t* tail = reinterpret_cast<const uint8_t*>(data + nblocks * 4);
  uint32_t k1 = 0;
  switch (len & 3) {
    case 3:
      k1 ^= static_cast<uint32_t>(tail[2]) << 16;
      [[fallthrough]];
    case 2:
      k1 ^= static_cast<uint32_t>(tail[1]) << 8;
      [[fallthrough]];
    case 1:
      k1 ^= static_cast<uint32_t>(tail[0]);
      k1 *= c1;
      k1 = (k1 << 15) | (k1 >> 17);
      k1 *= c2;
      h ^= k1;
  }

  h ^= static_cast<uint32_t>(len);
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

void BloomFilter::CreateFilter(const std::vector<std::string>& keys,
                              int bits_per_key, std::string* dst) {
  size_t bits = keys.size() * bits_per_key;
  if (bits < 64) {
    bits = 64;
  }

  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  int k = static_cast<int>(bits_per_key * 0.69);  // ln(2) ~ 0.69
  if (k < 1) k = 1;
  if (k > 30) k = 30;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, 0);
  char* array = &(*dst)[init_size];

  for (const auto& key : keys) {
    uint32_t h = MurmurHash3(key.data(), key.size(), 0xbc9f1d34);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int j = 0; j < k; ++j) {
      const uint32_t bitpos = h % bits;
      array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }

  dst->push_back(static_cast<char>(k));
}

bool BloomFilter::KeyMayMatch(const Slice& key, const Slice& filter) {
  const size_t len = filter.size();
  if (len < 2) {
    return true;
  }

  const char* array = filter.data();
  const size_t bits = (len - 1) * 8;
  const int k = static_cast<uint8_t>(array[len - 1]);
  if (k > 30) {
    return true;
  }

  uint32_t h = MurmurHash3(key.data(), key.size(), 0xbc9f1d34);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int j = 0; j < k; ++j) {
    const uint32_t bitpos = h % bits;
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace strata
