#include "compaction.h"

#include <unistd.h>
#include <algorithm>
#include "filename.h"
#include "merging_iterator.h"
#include "sstable_builder.h"

namespace strata {

Compaction::Compaction(const Options& options, int level)
    : level_(level), options_(options) {}

bool Compaction::IsBaseLevelForKey(const std::string& user_key,
                                  const Version* current) {
  for (int lvl = level_ + 2; lvl < kNumLevels; ++lvl) {
    const auto& files = current->Files(lvl);
    for (const auto& f : files) {
      if (user_key >= f.smallest_key && user_key <= f.largest_key) {
        return false;
      }
    }
  }
  return true;
}

std::unique_ptr<Compaction> Compaction::PickCompaction(const Options& options,
                                                       VersionSet* vset) {
  Version* current = vset->current();

  // Check Level 0 file count trigger
  if (current->NumLevelFiles(0) >= options.max_level0_files) {
    auto c = std::make_unique<Compaction>(options, 0);
    c->inputs_[0] = current->Files(0);

    if (c->inputs_[0].empty()) {
      return nullptr;
    }

    std::string min_key = c->inputs_[0][0].smallest_key;
    std::string max_key = c->inputs_[0][0].largest_key;
    for (const auto& f : c->inputs_[0]) {
      if (f.smallest_key < min_key) min_key = f.smallest_key;
      if (f.largest_key > max_key) max_key = f.largest_key;
    }

    // Find overlapping files in Level 1
    for (const auto& f : current->Files(1)) {
      if (!(f.largest_key < min_key || f.smallest_key > max_key)) {
        c->inputs_[1].push_back(f);
      }
    }
    return c;
  }

  // Check size-based compaction for Level 1..5
  for (int lvl = 1; lvl < kNumLevels - 1; ++lvl) {
    uint64_t limit = (1ULL << (20 + (lvl - 1) * 2));  // 1MB, 4MB, 16MB...
    if (current->LevelBytes(lvl) > limit && !current->Files(lvl).empty()) {
      auto c = std::make_unique<Compaction>(options, lvl);
      // Pick first file in level
      c->inputs_[0].push_back(current->Files(lvl)[0]);
      std::string min_key = c->inputs_[0][0].smallest_key;
      std::string max_key = c->inputs_[0][0].largest_key;

      for (const auto& f : current->Files(lvl + 1)) {
        if (!(f.largest_key < min_key || f.smallest_key > max_key)) {
          c->inputs_[1].push_back(f);
        }
      }
      return c;
    }
  }

  return nullptr;
}

Status DoCompaction(const Options& options, const std::string& dbname,
                    VersionSet* vset, Compaction* c, BlockCache* cache) {
  std::vector<std::unique_ptr<InternalIterator>> iters;

  // Open readers and iterators for inputs
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs(which)) {
      if (f.reader == nullptr) {
        std::string path = TableFileName(dbname, f.number);
        Status s = SSTableReader::Open(options, path, f.number, f.file_size, &f.reader);
        if (!s.ok()) {
          return s;
        }
      }
      iters.emplace_back(f.reader->NewIterator(cache));
    }
  }

  std::unique_ptr<InternalIterator> input_iter(NewMergingIterator(std::move(iters)));
  if (!input_iter) {
    return Status::OK();
  }

  struct OutputFile {
    uint64_t number;
    uint64_t file_size;
    std::string smallest_key;
    std::string largest_key;
  };
  std::vector<OutputFile> outputs;

  std::unique_ptr<SSTableBuilder> builder;
  uint64_t current_file_number = 0;

  auto OpenNewOutputFile = [&]() -> Status {
    current_file_number = vset->NewFileNumber();
    std::string path = TableFileName(dbname, current_file_number);
    builder = std::make_unique<SSTableBuilder>(options, path);
    return Status::OK();
  };

  auto FinishOutputFile = [&]() -> Status {
    if (builder && builder->NumEntries() > 0) {
      Status s = builder->Finish();
      if (!s.ok()) return s;
      OutputFile out;
      out.number = current_file_number;
      out.file_size = builder->FileSize();
      out.smallest_key = builder->SmallestKey();
      out.largest_key = builder->LargestKey();
      outputs.push_back(out);
    }
    builder.reset();
    return Status::OK();
  };

  input_iter->SeekToFirst();
  std::string last_key = "";
  bool has_last_key = false;

  while (input_iter->Valid()) {
    std::string key = input_iter->Key();
    SequenceNumber seq = input_iter->Seq();
    ValueType type = input_iter->Type();
    std::string value = input_iter->Value();

    bool is_duplicate = has_last_key && (key == last_key);
    if (!is_duplicate) {
      last_key = key;
      has_last_key = true;

      // Handle tombstone dropping if key does not exist in deeper levels
      bool drop = false;
      if (type == kTypeDeletion && c->IsBaseLevelForKey(key, vset->current())) {
        drop = true;
      }

      if (!drop) {
        if (!builder) {
          Status s = OpenNewOutputFile();
          if (!s.ok()) return s;
        }

        builder->Add(seq, type, key, value);

        if (builder->FileSize() >= options.max_file_size) {
          Status s = FinishOutputFile();
          if (!s.ok()) return s;
        }
      }
    }
    input_iter->Next();
  }

  Status s = FinishOutputFile();
  if (!s.ok()) return s;

  // Build VersionEdit
  VersionEdit edit;
  for (const auto& f : c->inputs(0)) {
    edit.DeleteFile(c->level(), f.number);
  }
  for (const auto& f : c->inputs(1)) {
    edit.DeleteFile(c->level() + 1, f.number);
  }
  for (const auto& out : outputs) {
    edit.AddFile(c->level() + 1, out.number, out.file_size, out.smallest_key,
                 out.largest_key);
  }

  s = vset->LogAndApply(&edit);
  if (!s.ok()) return s;

  // Safely delete obsolete files
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs(which)) {
      std::string path = TableFileName(dbname, f.number);
      ::unlink(path.c_str());
    }
  }

  return Status::OK();
}

}  // namespace strata
