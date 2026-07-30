#include <fcntl.h>
#include <sys/stat.h>
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

void TestWalMidLogCorruptionFails() {
  std::cout << "Running TestWalMidLogCorruptionFails..." << std::endl;
  std::string wal_path = "/tmp/strata_wal_test_mid_corrupt.log";
  ::unlink(wal_path.c_str());

  {
    strata::WalWriter writer(wal_path);
    ASSERT_OK(writer.AppendRecord(1, strata::kTypeValue, "alpha", "val_1", false));
    ASSERT_OK(writer.AppendRecord(2, strata::kTypeValue, "bravo", "val_2", false));
    ASSERT_OK(writer.AppendRecord(3, strata::kTypeValue, "charlie", "val_3", false));
    ASSERT_OK(writer.Close());
  }

  // Corrupt record 2 (in the middle of the log) by flipping a byte in its payload
  // Record 1 size: 8-byte header + (1 + 8 + 4 + 5 + 4 + 5) = 8 + 27 = 35 bytes.
  // Record 2 starts at byte offset 35. Its CRC is at bytes 35..38.
  int fd = ::open(wal_path.c_str(), O_RDWR);
  ASSERT_TRUE(fd >= 0);
  char bad_byte = 0x7F;
  ASSERT_EQ(::pwrite(fd, &bad_byte, 1, 36), 1);  // Corrupt record 2's CRC
  ::close(fd);

  // WalReader even with allow_partial_eof=true MUST reject mid-log corruption
  {
    strata::WalReader reader(wal_path);
    strata::SequenceNumber seq = 0;
    strata::ValueType type = strata::kTypeValue;
    std::string key, val;

    // Record 1 must succeed
    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val, true));
    ASSERT_EQ(key, "alpha");

    // Record 2 must fail loudly as Corruption, NOT NotFound
    strata::Status s = reader.ReadRecord(&seq, &type, &key, &val, true);
    ASSERT_TRUE(s.IsCorruption());
  }

  ::unlink(wal_path.c_str());
}

void TestWalTailCorruptionRecoversPrefix() {
  std::cout << "Running TestWalTailCorruptionRecoversPrefix..." << std::endl;
  std::string wal_path = "/tmp/strata_wal_test_tail_corrupt.log";
  ::unlink(wal_path.c_str());

  {
    strata::WalWriter writer(wal_path);
    ASSERT_OK(writer.AppendRecord(1, strata::kTypeValue, "alpha", "val_1", false));
    ASSERT_OK(writer.AppendRecord(2, strata::kTypeValue, "bravo", "val_2", false));
    ASSERT_OK(writer.AppendRecord(3, strata::kTypeValue, "charlie", "val_3", false));
    ASSERT_OK(writer.Close());
  }

  // Corrupt record 3 at the very tail of the file
  struct stat st;
  ASSERT_EQ(::stat(wal_path.c_str(), &st), 0);
  int fd = ::open(wal_path.c_str(), O_RDWR);
  ASSERT_TRUE(fd >= 0);
  char bad_byte = 0x5A;
  ASSERT_EQ(::pwrite(fd, &bad_byte, 1, st.st_size - 5), 1);  // Corrupt tail payload
  ::close(fd);

  // WalReader with allow_partial_eof=true should recover records 1 and 2, then stop cleanly at EOF
  {
    strata::WalReader reader(wal_path);
    strata::SequenceNumber seq = 0;
    strata::ValueType type = strata::kTypeValue;
    std::string key, val;

    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val, true));
    ASSERT_EQ(key, "alpha");
    ASSERT_OK(reader.ReadRecord(&seq, &type, &key, &val, true));
    ASSERT_EQ(key, "bravo");

    // Record 3 at tail is treated as torn write at EOF (NotFound)
    strata::Status s = reader.ReadRecord(&seq, &type, &key, &val, true);
    ASSERT_TRUE(s.IsNotFound());
  }

  ::unlink(wal_path.c_str());
}

}  // namespace

int main() {
  TestWalBasicAppendRead();
  TestWalPartialEofRecovery();
  TestWalMidLogCorruptionFails();
  TestWalTailCorruptionRecoversPrefix();
  std::cout << "All WAL tests passed!" << std::endl;
  return 0;
}
