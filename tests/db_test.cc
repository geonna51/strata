#include <cassert>
#include <iostream>
#include "strata/db.h"
#include "test_util.h"

int main() {
  std::cout << "Running Phase 0 DB open/stub test..." << std::endl;

  strata::Options options;
  strata::DB* db = nullptr;
  strata::Status s = strata::DB::Open(options, "/tmp/strata_test_phase0", &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != nullptr);

  delete db;
  std::cout << "Phase 0 DB open/stub test passed." << std::endl;
  return 0;
}
