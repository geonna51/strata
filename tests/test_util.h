#pragma once

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace strata {
namespace test {

#define ASSERT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Assertion failed: "      \
                << #cond << std::endl;                                         \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Assertion failed: "      \
                << #a << " == " << #b << " (" << (a) << " vs " << (b) << ")"  \
                << std::endl;                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_NE(a, b)                                                        \
  do {                                                                         \
    if ((a) == (b)) {                                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Assertion failed: "      \
                << #a << " != " << #b << std::endl;                            \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_OK(status)                                                      \
  do {                                                                         \
    const auto& _status_eval = (status);                                       \
    if (!_status_eval.ok()) {                                                  \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Expected OK, got: "      \
                << _status_eval.ToString() << std::endl;                       \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_NOT_FOUND(status)                                               \
  do {                                                                         \
    const auto& _status_eval = (status);                                       \
    if (!_status_eval.IsNotFound()) {                                          \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Expected NotFound, got: "\
      << _status_eval.ToString() << std::endl;                                 \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

inline void CleanDir(const std::string& path) {
  int rc = ::system(("rm -rf " + path).c_str());
  (void)rc;
}

}  // namespace test
}  // namespace strata

using strata::test::CleanDir;
