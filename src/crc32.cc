#include "crc32.h"

namespace strata {

static const uint32_t* GetCrcTable() {
  static uint32_t table[256];
  static bool initialized = false;
  if (!initialized) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    initialized = true;
  }
  return table;
}

uint32_t Crc32(const void* data, size_t n, uint32_t seed) {
  const uint32_t* table = GetCrcTable();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = seed ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

}  // namespace strata
