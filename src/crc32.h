#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

uint32_t Crc32(const void* data, size_t n, uint32_t seed = 0);

}  // namespace strata
