#pragma once

#include <string>
#include "strata/status.h"

namespace strata {

class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  // An iterator is either positioned at a key/value pair, or not valid.
  virtual bool Valid() const = 0;

  // Position at the first key in the source.
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source.
  virtual void SeekToLast() = 0;

  // Position at the first key that is >= target.
  virtual void Seek(const std::string& target) = 0;

  // Moves to the next entry.
  // REQUIRES: Valid()
  virtual void Next() = 0;

  // Moves to the previous entry.
  // REQUIRES: Valid()
  virtual void Prev() = 0;

  // Return the key for the current entry.
  // REQUIRES: Valid()
  virtual std::string Key() const = 0;

  // Return the value for the current entry.
  // REQUIRES: Valid()
  virtual std::string Value() const = 0;

  // If an error occurred, return it. Else return OK.
  virtual Status status() const = 0;
};

}  // namespace strata
