#pragma once

#include <cstdint>
#include <string>
#include "strata/slice.h"

namespace strata {

using SequenceNumber = uint64_t;

static constexpr SequenceNumber kMaxSequenceNumber = ((0x1ULL << 56) - 1);

enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
};

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() : sequence(0), type(kTypeValue) {}
  ParsedInternalKey(Slice u, SequenceNumber s, ValueType t)
      : user_key(u), sequence(s), type(t) {}
};

inline int CompareInternalKey(const Slice& a_user_key, SequenceNumber a_seq,
                              const Slice& b_user_key, SequenceNumber b_seq) {
  int r = a_user_key.compare(b_user_key);
  if (r != 0) {
    return r;
  }
  if (a_seq > b_seq) {
    return -1;
  }
  if (a_seq < b_seq) {
    return +1;
  }
  return 0;
}

}  // namespace strata
