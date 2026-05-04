#pragma once

#include <string>
#include "strata/db.h"

namespace strata {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  ~DBImpl() override;

  Status Put(const std::string& key, const std::string& value) override;
  Status Get(const std::string& key, std::string* value) override;
  Status Delete(const std::string& key) override;
  Iterator* NewIterator() override;

 private:
  Options options_;
  std::string dbname_;
};

}  // namespace strata
