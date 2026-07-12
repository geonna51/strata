#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "strata/options.h"
#include "strata/status.h"
#include "version_set.h"

namespace strata {

class Compaction {
 public:
  Compaction(const Options& options, int level);

  int level() const { return level_; }
  const std::vector<FileMetaData>& inputs(int which) const { return inputs_[which]; }

  // True if key does not exist in any level > output_level.
  bool IsBaseLevelForKey(const std::string& user_key, const Version* current);

  // Setup inputs from current version.
  static std::unique_ptr<Compaction> PickCompaction(const Options& options,
                                                    VersionSet* vset);

 private:
  int level_;
  Options options_;
  // inputs_[0] are files from level_, inputs_[1] are overlapping files from level_ + 1
  std::vector<FileMetaData> inputs_[2];
};

Status DoCompaction(const Options& options, const std::string& dbname,
                    VersionSet* vset, Compaction* c, BlockCache* cache);

}  // namespace strata
