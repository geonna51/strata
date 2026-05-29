#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace strata {

inline void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32(buf, value);
  dst->append(buf, 4);
}

inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);
  for (int i = 0; i < 8; ++i) {
    buffer[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64(buf, value);
  dst->append(buf, 8);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= (static_cast<uint64_t>(buffer[i]) << (i * 8));
  }
  return result;
}

}  // namespace strata
