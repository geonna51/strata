#pragma once

#include <string>
#include "strata/iterator.h"
#include "strata/options.h"
#include "strata/status.h"

namespace strata {

class DB {
 public:
  // Open the database with the specified "name" (directory path).
  // Stores a pointer to a heap-allocated DB in *dbptr on success.
  static Status Open(const Options& options, const std::string& name, DB** dbptr);

  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Set the database entry for "key" to "value".
  virtual Status Put(const std::string& key, const std::string& value) = 0;

  // If the database contains an entry for "key", store the corresponding
  // value in *value and return OK. If not found, return NotFound.
  virtual Status Get(const std::string& key, std::string* value) = 0;

  // Remove the database entry for "key".
  virtual Status Delete(const std::string& key) = 0;

  // Return an iterator over the contents of the database.
  // The caller is responsible for deleting the returned iterator.
  virtual Iterator* NewIterator() = 0;
};

}  // namespace strata
