#include "version_set.h"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include "coding.h"
#include "filename.h"

namespace strata {

enum EditTag {
  kTagComparator = 1,
  kTagLogNumber = 2,
  kTagNextFileNumber = 3,
  kTagLastSequence = 4,
  kTagDeletedFile = 5,
  kTagNewFile = 6,
};

void VersionEdit::Clear() {
  has_comparator_ = false;
  comparator_name_.clear();
  has_log_number_ = false;
  log_number_ = 0;
  has_next_file_number_ = false;
  next_file_number_ = 0;
  has_last_sequence_ = false;
  last_sequence_ = 0;
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::SetComparatorName(const std::string& name) {
  has_comparator_ = true;
  comparator_name_ = name;
}

void VersionEdit::SetLogNumber(uint64_t num) {
  has_log_number_ = true;
  log_number_ = num;
}

void VersionEdit::SetNextFile(uint64_t num) {
  has_next_file_number_ = true;
  next_file_number_ = num;
}

void VersionEdit::SetLastSequence(uint64_t seq) {
  has_last_sequence_ = true;
  last_sequence_ = seq;
}

void VersionEdit::DeleteFile(int level, uint64_t file) {
  deleted_files_.insert({level, file});
}

void VersionEdit::AddFile(int level, uint64_t file, uint64_t file_size,
                         const std::string& smallest,
                         const std::string& largest) {
  FileMetaData meta;
  meta.number = file;
  meta.file_size = file_size;
  meta.smallest_key = smallest;
  meta.largest_key = largest;
  new_files_.push_back({level, meta});
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    dst->push_back(static_cast<char>(kTagComparator));
    PutFixed32(dst, static_cast<uint32_t>(comparator_name_.size()));
    dst->append(comparator_name_);
  }
  if (has_log_number_) {
    dst->push_back(static_cast<char>(kTagLogNumber));
    PutFixed64(dst, log_number_);
  }
  if (has_next_file_number_) {
    dst->push_back(static_cast<char>(kTagNextFileNumber));
    PutFixed64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    dst->push_back(static_cast<char>(kTagLastSequence));
    PutFixed64(dst, last_sequence_);
  }
  for (const auto& d : deleted_files_) {
    dst->push_back(static_cast<char>(kTagDeletedFile));
    PutFixed32(dst, d.first);
    PutFixed64(dst, d.second);
  }
  for (const auto& n : new_files_) {
    dst->push_back(static_cast<char>(kTagNewFile));
    PutFixed32(dst, n.first);
    PutFixed64(dst, n.second.number);
    PutFixed64(dst, n.second.file_size);
    PutFixed32(dst, static_cast<uint32_t>(n.second.smallest_key.size()));
    dst->append(n.second.smallest_key);
    PutFixed32(dst, static_cast<uint32_t>(n.second.largest_key.size()));
    dst->append(n.second.largest_key);
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;
  while (!input.empty()) {
    uint8_t tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    switch (tag) {
      case kTagComparator: {
        if (input.size() < 4) return Status::Corruption("Bad comparator in edit");
        uint32_t len = DecodeFixed32(input.data());
        input.remove_prefix(4);
        if (input.size() < len) return Status::Corruption("Truncated comparator");
        comparator_name_ = std::string(input.data(), len);
        input.remove_prefix(len);
        has_comparator_ = true;
        break;
      }
      case kTagLogNumber: {
        if (input.size() < 8) return Status::Corruption("Bad log number in edit");
        log_number_ = DecodeFixed64(input.data());
        input.remove_prefix(8);
        has_log_number_ = true;
        break;
      }
      case kTagNextFileNumber: {
        if (input.size() < 8) return Status::Corruption("Bad next file in edit");
        next_file_number_ = DecodeFixed64(input.data());
        input.remove_prefix(8);
        has_next_file_number_ = true;
        break;
      }
      case kTagLastSequence: {
        if (input.size() < 8) return Status::Corruption("Bad last sequence in edit");
        last_sequence_ = DecodeFixed64(input.data());
        input.remove_prefix(8);
        has_last_sequence_ = true;
        break;
      }
      case kTagDeletedFile: {
        if (input.size() < 12) return Status::Corruption("Bad deleted file in edit");
        int level = DecodeFixed32(input.data());
        uint64_t file = DecodeFixed64(input.data() + 4);
        input.remove_prefix(12);
        deleted_files_.insert({level, file});
        break;
      }
      case kTagNewFile: {
        if (input.size() < 20) return Status::Corruption("Bad new file in edit");
        int level = DecodeFixed32(input.data());
        FileMetaData meta;
        meta.number = DecodeFixed64(input.data() + 4);
        meta.file_size = DecodeFixed64(input.data() + 12);
        input.remove_prefix(20);

        if (input.size() < 4) return Status::Corruption("Bad smallest key len");
        uint32_t slen = DecodeFixed32(input.data());
        input.remove_prefix(4);
        if (input.size() < slen) return Status::Corruption("Truncated smallest key");
        meta.smallest_key = std::string(input.data(), slen);
        input.remove_prefix(slen);

        if (input.size() < 4) return Status::Corruption("Bad largest key len");
        uint32_t llen = DecodeFixed32(input.data());
        input.remove_prefix(4);
        if (input.size() < llen) return Status::Corruption("Truncated largest key");
        meta.largest_key = std::string(input.data(), llen);
        input.remove_prefix(llen);

        new_files_.push_back({level, meta});
        break;
      }
      default:
        return Status::Corruption("Unknown tag in VersionEdit");
    }
  }
  return Status::OK();
}

Version::Version(std::string dbname) : dbname_(std::move(dbname)) {}
Version::~Version() = default;

uint64_t Version::LevelBytes(int level) const {
  uint64_t total = 0;
  for (const auto& f : files_[level]) {
    total += f.file_size;
  }
  return total;
}

bool Version::Get(const Options& options, const Slice& user_key,
                  SequenceNumber seq, std::string* value, Status* s,
                  BlockCache* cache) {
  std::string ukey = user_key.ToString();

  // Search Level 0 files (newest to oldest)
  for (const auto& f : files_[0]) {
    if (ukey < f.smallest_key || ukey > f.largest_key) {
      continue;
    }
    if (f.reader == nullptr) {
      std::string path = TableFileName(dbname_, f.number);
      Status st = SSTableReader::Open(options, path, f.number, f.file_size, &f.reader);
      if (!st.ok()) {
        *s = st;
        return true;
      }
    }
    if (f.reader->Get(user_key, seq, value, s, cache)) {
      return true;
    }
  }

  // Search Levels 1 through 6
  for (int level = 1; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;

    int left = 0;
    int right = static_cast<int>(files_[level].size()) - 1;
    int candidate = -1;

    while (left <= right) {
      int mid = left + (right - left) / 2;
      const auto& f = files_[level][mid];
      if (f.largest_key < ukey) {
        left = mid + 1;
      } else if (f.smallest_key > ukey) {
        right = mid - 1;
      } else {
        candidate = mid;
        break;
      }
    }

    if (candidate >= 0) {
      const auto& f = files_[level][candidate];
      if (f.reader == nullptr) {
        std::string path = TableFileName(dbname_, f.number);
        Status st = SSTableReader::Open(options, path, f.number, f.file_size, &f.reader);
        if (!st.ok()) {
          *s = st;
          return true;
        }
      }
      if (f.reader->Get(user_key, seq, value, s, cache)) {
        return true;
      }
    }
  }

  return false;
}

void Version::AddIterators(const Options& options, BlockCache* cache,
                           std::vector<Iterator*>* iters) {
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : files_[level]) {
      if (f.reader == nullptr) {
        std::string path = TableFileName(dbname_, f.number);
        Status st = SSTableReader::Open(options, path, f.number, f.file_size, &f.reader);
        if (!st.ok()) {
          continue;
        }
      }
      iters->push_back(f.reader->NewIterator(cache));
    }
  }
}

VersionSet::VersionSet(std::string dbname, const Options* options,
                       BlockCache* cache)
    : dbname_(std::move(dbname)),
      options_(options),
      cache_(cache),
      current_(std::make_shared<Version>(dbname_)),
      next_file_number_(2),
      manifest_file_number_(0),
      last_sequence_(0),
      log_number_(0),
      descriptor_log_(nullptr) {}

VersionSet::~VersionSet() = default;

void VersionSet::AddLiveFiles(std::set<uint64_t>* live_files) const {
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : current_->files_[level]) {
      live_files->insert(f.number);
    }
  }
}

Status VersionSet::WriteSnapshot(WalWriter* writer) {
  VersionEdit edit;
  edit.SetComparatorName("byte_comparator");
  edit.SetNextFile(next_file_number_);
  edit.SetLastSequence(last_sequence_);
  edit.SetLogNumber(log_number_);

  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : current_->files_[level]) {
      edit.AddFile(level, f.number, f.file_size, f.smallest_key, f.largest_key);
    }
  }

  std::string record;
  edit.EncodeTo(&record);
  return writer->AppendRecord(0, kTypeValue, "MANIFEST_ENTRY", record, true);
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
  }
  if (!edit->has_next_file_number_) {
    edit->SetNextFile(next_file_number_);
  }
  if (!edit->has_last_sequence_) {
    edit->SetLastSequence(last_sequence_);
  }

  auto v = std::make_shared<Version>(dbname_);
  for (int level = 0; level < kNumLevels; ++level) {
    // Copy existing files excluding deleted
    for (const auto& f : current_->files_[level]) {
      if (edit->deleted_files_.count({level, f.number}) == 0) {
        v->files_[level].push_back(f);
      }
    }
  }

  // Add new files
  for (const auto& item : edit->new_files_) {
    v->files_[item.first].push_back(item.second);
  }

  // Sort Level 0 by file number descending (newest first)
  std::sort(v->files_[0].begin(), v->files_[0].end(),
            [](const FileMetaData& a, const FileMetaData& b) {
              return a.number > b.number;
            });

  // Sort Levels 1..6 by smallest_key ascending
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(v->files_[level].begin(), v->files_[level].end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return a.smallest_key < b.smallest_key;
              });
  }

  // Initialize MANIFEST writer if not already created
  std::string manifest_path;
  if (descriptor_log_ == nullptr) {
    manifest_file_number_ = NewFileNumber();
    manifest_path = ManifestFileName(dbname_, manifest_file_number_);
    descriptor_log_ = std::make_unique<WalWriter>(manifest_path);
    Status s = WriteSnapshot(descriptor_log_.get());
    if (!s.ok()) {
      descriptor_log_.reset();
      return s;
    }

    // Atomically write CURRENT file pointing to MANIFEST filename
    std::string current_tmp = CurrentFileName(dbname_) + ".tmp";
    std::ofstream out(current_tmp);
    if (!out.is_open()) {
      return Status::IOError("Failed to create " + current_tmp);
    }
    out << "MANIFEST-" << (manifest_file_number_ < 10 ? "00000" : "0000")
        << manifest_file_number_ << "\n";
    out.close();
    if (::rename(current_tmp.c_str(), CurrentFileName(dbname_).c_str()) != 0) {
      return Status::IOError("Failed to update CURRENT file");
    }
  }

  // Append edit to descriptor log
  std::string record;
  edit->EncodeTo(&record);
  Status s = descriptor_log_->AppendRecord(0, kTypeValue, "MANIFEST_ENTRY", record, true);
  if (!s.ok()) {
    return s;
  }

  // Install new version
  current_ = v;
  if (edit->has_log_number_) log_number_ = edit->log_number_;
  if (edit->has_last_sequence_) last_sequence_ = edit->last_sequence_;

  return Status::OK();
}

Status VersionSet::Recover() {
  // Read CURRENT file
  std::ifstream current_in(CurrentFileName(dbname_));
  if (!current_in.is_open()) {
    return Status::NotFound("CURRENT file not found");
  }

  std::string manifest_name;
  std::getline(current_in, manifest_name);
  if (manifest_name.empty()) {
    return Status::Corruption("Empty CURRENT file");
  }

  std::string manifest_path = dbname_ + "/" + manifest_name;
  WalReader reader(manifest_path);

  auto v = std::make_shared<Version>(dbname_);
  SequenceNumber seq = 0;
  ValueType type = kTypeValue;
  std::string k, edit_record;

  uint64_t max_file_num = 0;
  uint64_t max_seq = 0;
  uint64_t log_num = 0;

  while (reader.ReadRecord(&seq, &type, &k, &edit_record).ok()) {
    VersionEdit edit;
    Status s = edit.DecodeFrom(Slice(edit_record));
    if (!s.ok()) {
      return s;
    }

    if (edit.has_next_file_number_ && edit.next_file_number_ > max_file_num) {
      max_file_num = edit.next_file_number_;
    }
    if (edit.has_last_sequence_ && edit.last_sequence_ > max_seq) {
      max_seq = edit.last_sequence_;
    }
    if (edit.has_log_number_) {
      log_num = edit.log_number_;
    }

    // Apply deletions
    for (int level = 0; level < kNumLevels; ++level) {
      std::vector<FileMetaData> retained;
      for (const auto& f : v->files_[level]) {
        if (edit.deleted_files_.count({level, f.number}) == 0) {
          retained.push_back(f);
        }
      }
      v->files_[level] = std::move(retained);
    }

    // Apply additions
    for (const auto& item : edit.new_files_) {
      v->files_[item.first].push_back(item.second);
      if (item.second.number >= max_file_num) {
        max_file_num = item.second.number + 1;
      }
    }
  }

  // Sort Level 0 by file number descending
  std::sort(v->files_[0].begin(), v->files_[0].end(),
            [](const FileMetaData& a, const FileMetaData& b) {
              return a.number > b.number;
            });

  // Sort Levels 1..6 by smallest_key ascending
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(v->files_[level].begin(), v->files_[level].end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return a.smallest_key < b.smallest_key;
              });
  }

  current_ = v;
  next_file_number_ = max_file_num > 0 ? max_file_num : 2;
  last_sequence_ = max_seq;
  log_number_ = log_num;

  return Status::OK();
}

}  // namespace strata
