#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "test_util.h"
#include "version_set.h"

namespace {

void TestVersionEditEncodeDecode() {
  std::cout << "Running TestVersionEditEncodeDecode..." << std::endl;
  strata::VersionEdit edit;
  edit.SetComparatorName("leveldb.BytewiseComparator");
  edit.SetLogNumber(100);
  edit.SetNextFile(200);
  edit.SetLastSequence(1000);
  edit.DeleteFile(0, 5);
  edit.DeleteFile(1, 10);
  edit.AddFile(0, 15, 4096, "a", "m");
  edit.AddFile(1, 16, 8192, "n", "z");

  std::string encoded;
  edit.EncodeTo(&encoded);

  strata::VersionEdit decoded;
  ASSERT_OK(decoded.DecodeFrom(strata::Slice(encoded)));

  ASSERT_TRUE(decoded.has_comparator_);
  ASSERT_EQ(decoded.comparator_name_, "leveldb.BytewiseComparator");
  ASSERT_TRUE(decoded.has_log_number_);
  ASSERT_EQ(decoded.log_number_, 100);
  ASSERT_TRUE(decoded.has_next_file_number_);
  ASSERT_EQ(decoded.next_file_number_, 200);
  ASSERT_TRUE(decoded.has_last_sequence_);
  ASSERT_EQ(decoded.last_sequence_, 1000);

  ASSERT_EQ(decoded.deleted_files_.size(), 2);
  ASSERT_EQ(decoded.deleted_files_.count({0, 5}), 1);
  ASSERT_EQ(decoded.deleted_files_.count({1, 10}), 1);

  ASSERT_EQ(decoded.new_files_.size(), 2);
  ASSERT_EQ(decoded.new_files_[0].first, 0);
  ASSERT_EQ(decoded.new_files_[0].second.number, 15);
  ASSERT_EQ(decoded.new_files_[0].second.file_size, 4096);
  ASSERT_EQ(decoded.new_files_[0].second.smallest_key, "a");
  ASSERT_EQ(decoded.new_files_[0].second.largest_key, "m");

  ASSERT_EQ(decoded.new_files_[1].first, 1);
  ASSERT_EQ(decoded.new_files_[1].second.number, 16);
  ASSERT_EQ(decoded.new_files_[1].second.file_size, 8192);
  ASSERT_EQ(decoded.new_files_[1].second.smallest_key, "n");
  ASSERT_EQ(decoded.new_files_[1].second.largest_key, "z");
}

void TestVersionSetLogApplyAndRecover() {
  std::cout << "Running TestVersionSetLogApplyAndRecover..." << std::endl;
  std::string dbpath = "/tmp/strata_test_version_set_dir";
  CleanDir(dbpath);
  ::mkdir(dbpath.c_str(), 0755);

  strata::Options options;
  strata::BlockCache cache(1024 * 1024);

  {
    strata::VersionSet vset(dbpath, &options, &cache);
    strata::VersionEdit edit;
    edit.SetLogNumber(1);
    edit.SetNextFile(10);
    edit.SetLastSequence(500);
    edit.AddFile(0, 3, 1024, "apple", "banana");
    edit.AddFile(0, 4, 2048, "carrot", "date");
    edit.AddFile(1, 5, 4096, "egg", "fig");
    ASSERT_OK(vset.LogAndApply(&edit));

    ASSERT_EQ(vset.current()->NumLevelFiles(0), 2);
    ASSERT_EQ(vset.current()->NumLevelFiles(1), 1);
    ASSERT_EQ(vset.LastSequence(), 500);

    // Apply second edit: delete file 3 from L0, add file 6 to L1
    strata::VersionEdit edit2;
    edit2.DeleteFile(0, 3);
    edit2.AddFile(1, 6, 4096, "grape", "kiwi");
    edit2.SetLastSequence(600);
    ASSERT_OK(vset.LogAndApply(&edit2));

    ASSERT_EQ(vset.current()->NumLevelFiles(0), 1);
    ASSERT_EQ(vset.current()->NumLevelFiles(1), 2);
    ASSERT_EQ(vset.LastSequence(), 600);
  }

  // Recover state in a new VersionSet instance
  {
    strata::VersionSet recovered_vset(dbpath, &options, &cache);
    ASSERT_OK(recovered_vset.Recover());

    ASSERT_EQ(recovered_vset.current()->NumLevelFiles(0), 1);
    ASSERT_EQ(recovered_vset.current()->NumLevelFiles(1), 2);
    ASSERT_EQ(recovered_vset.LastSequence(), 600);

    const auto& l0 = recovered_vset.current()->Files(0);
    ASSERT_EQ(l0[0].number, 4);

    const auto& l1 = recovered_vset.current()->Files(1);
    ASSERT_EQ(l1[0].number, 5);
    ASSERT_EQ(l1[1].number, 6);
  }

  CleanDir(dbpath);
}

}  // namespace

int main() {
  TestVersionEditEncodeDecode();
  TestVersionSetLogApplyAndRecover();
  std::cout << "All VersionSet tests passed!" << std::endl;
  return 0;
}
