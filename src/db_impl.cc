#include "db_impl.h"

namespace strata {

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = new DBImpl(options, name);
  return Status::OK();
}

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : options_(options), dbname_(dbname) {}

DBImpl::~DBImpl() = default;

Status DBImpl::Put(const std::string& /*key*/, const std::string& /*value*/) {
  return Status::OK();
}

Status DBImpl::Get(const std::string& /*key*/, std::string* /*value*/) {
  return Status::NotFound();
}

Status DBImpl::Delete(const std::string& /*key*/) {
  return Status::OK();
}

Iterator* DBImpl::NewIterator() {
  return nullptr;
}

}  // namespace strata
