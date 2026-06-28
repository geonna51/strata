#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace strata {

inline std::string MakeFileName(const std::string& dbname, uint64_t number,
                                const char* prefix, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%s%06llu.%s", prefix,
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

inline std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "", "log");
}

inline std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "", "sst");
}

inline std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

inline std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

inline std::string LockFileName(const std::string& dbname) {
  return dbname + "/LOCK";
}

}  // namespace strata
