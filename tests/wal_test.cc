#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>
#include "test_util.h"
#include "wal.h"

namespace {

void TestWalBasicAppendRead() {
  std::cout << "Running TestWalBasicAppendRead..." << std::endl;
  std::string wal_path = "/tmp/strata_wal_test_basic.log";
  ::unlink(wal_path.c_str());

  {
    strata::WalWriter writer(wal_path);
    ASSERT_OK(writer.AppendRecord(1, strata::kTypeValue, "k1", "v1", false));
    ASSERT_OK(writer.AppendRecord(2, strata::kTypeValue, "k2", "v2", true));
    ASSERT_OK(writer.AppendRecord(3, strata::kTypeDeletion, "k1", "", false));
    ASSERT_OK(writer.Close());
  }

  {
    strata::WalReader reader(wal_path);
    strata::SequenceNumber seq = 0;
    strata::ValueType type = strata::kTypeValue;
    std::string key, val;

    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val));
    ASSERT_EQ(seq, 1);
    ASSERT_EQ(type, strata::kTypeValue);
    ASSERT_EQ(key, "k1");
    ASSERT_EQ(val, "v1");

    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val));
    ASSERT_EQ(seq, 2);
    ASSERT_EQ(type, strata::kTypeValue);
    ASSERT_EQ(key, "k2");
    ASSERT_EQ(val, "v2");

    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val));
    ASSERT_EQ(seq, 3);
    ASSERT_EQ(type, strata::kTypeDeletion);
    ASSERT_EQ(key, "k1");
    ASSERT_EQ(val, "");

    // Next read should return NotFound (EOF)
    strata::Status s = reader.ReadRecord(&seq, &type, &key, &val);
    ASSERT_TRUE(s.IsNotFound());
  }

  ::unlink(wal_path.c_str());
}

void TestWalPartialEofRecovery() {
  std::cout << "Running TestWalPartialEofRecovery..." << std::endl;
  std::string wal_path = "/tmp/strata_wal_test_crash.log";
  ::unlink(wal_path.c_str());

  {
    strata::WalWriter writer(wal_path);
    ASSERT_OK(writer.AppendRecord(1, strata::kTypeValue, "key_a", "val_a", false));
    ASSERT_OK(writer.AppendRecord(2, strata::kTypeValue, "key_b", "val_b", false));
    ASSERT_OK(writer.Close());
  }

  // Simulate a crash mid-write by appending junk trailing bytes
  int fd = ::open(wal_path.c_str(), O_WRONLY | O_APPEND);
  ASSERT_TRUE(fd >= 0);
  char partial_bytes[] = "partial junk header fragment";
  ASSERT_TRUE(::write(fd, partial_bytes, sizeof(partial_bytes)) > 0);
  ::close(fd);

  // WalReader with allow_partial_eof=true should cleanly recover the 2 valid records
  {
    strata::WalReader reader(wal_path);
    strata::SequenceNumber seq = 0;
    strata::ValueType type = strata::kTypeValue;
    std::string key, val;

    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val, true));
    ASSERT_EQ(key, "key_a");
    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val, true));
    ASSERT_EQ(key, "key_b");

    // The trailing partial write is treated as EOF
    strata::Status s = reader.ReadRecord(&seq, &type, &key, &val, true);
    ASSERT_TRUE(s.IsNotFound());
  }

  ::unlink(wal_path.c_str());
}

}  // namespace

int main() {
  TestWalBasicAppendRead();
  TestWalPartialEofRecovery();
  std::cout << "All WAL tests passed!" << std::endl;
  return 0;
}
