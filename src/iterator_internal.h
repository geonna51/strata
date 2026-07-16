#pragma once

#include "format.h"
#include "strata/iterator.h"

namespace strata {

class InternalIterator : public Iterator {
 public:
  ~InternalIterator() override = default;

  virtual SequenceNumber Seq() const = 0;
  virtual ValueType Type() const = 0;
};

}  // namespace strata
