#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "strata/db.h"
#include "strata/options.h"

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <write|verify> <dbpath> [num_keys] [ack_file] [sync]" << std::endl;
    return 1;
  }

  std::string mode = argv[1];
  std::string dbpath = argv[2];

  if (mode == "write") {
    int num_keys = (argc > 3) ? std::stoi(argv[3]) : 100000;
    std::string ack_file = (argc > 4) ? argv[4] : "/tmp/strata_crash_ack.txt";
    bool sync = (argc > 5) ? (std::stoi(argv[5]) != 0) : true;

    int ack_fd = ::open(ack_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (ack_fd < 0) {
      std::cerr << "Failed to open ack file: " << ack_file << std::endl;
      return 1;
    }

    strata::Options options;
    options.create_if_missing = true;
    options.sync = sync;
    // Small buffer size to induce flushes and WAL rollovers while writing
    options.write_buffer_size = 64 * 1024;

    strata::DB* db = nullptr;
    strata::Status s = strata::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
      std::cerr << "DB::Open failed: " << s.ToString() << std::endl;
      return 1;
    }

    std::cout << "READY" << std::endl;
    std::cout.flush();

    for (int i = 0; i < num_keys; ++i) {
      char key[32];
      snprintf(key, sizeof(key), "user:%08d", i);
      std::string val = "payload_data_for_user_" + std::to_string(i);

      s = db->Put(key, val);
      if (!s.ok()) {
        std::cerr << "Put failed at " << i << ": " << s.ToString() << std::endl;
        delete db;
        return 1;
      }

      // Record acknowledged write with newline
      std::string ack = std::to_string(i) + "\n";
      ssize_t written = ::write(ack_fd, ack.data(), ack.size());
      if (written < 0) {
        std::cerr << "Ack write failed" << std::endl;
      }
      ::fsync(ack_fd);

      if ((i + 1) % 5000 == 0) {
        std::cout << "Wrote " << (i + 1) << " keys..." << std::endl;
        std::cout.flush();
      }
    }

    delete db;
    ::close(ack_fd);
    std::cout << "Finished writing all " << num_keys << " keys." << std::endl;
    return 0;

  } else if (mode == "verify") {
    std::string ack_file = (argc > 4) ? argv[4] : "/tmp/strata_crash_ack.txt";

    std::ifstream in(ack_file);
    if (!in.is_open()) {
      std::cerr << "Failed to open ack file for verify: " << ack_file << std::endl;
      return 1;
    }

    std::vector<int> acked_keys;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        acked_keys.push_back(std::stoi(line));
      }
    }
    in.close();

    std::cout << "Verifying " << acked_keys.size() << " acknowledged writes after crash..." << std::endl;

    strata::Options options;
    options.create_if_missing = false;

    strata::DB* db = nullptr;
    strata::Status s = strata::DB::Open(options, dbpath, &db);
    if (!s.ok()) {
      std::cerr << "Recovery DB::Open failed: " << s.ToString() << std::endl;
      return 1;
    }

    int verified = 0;
    int missing = 0;
    int corrupted = 0;

    for (int id : acked_keys) {
      char key[32];
      snprintf(key, sizeof(key), "user:%08d", id);
      std::string expected_val = "payload_data_for_user_" + std::to_string(id);

      std::string actual_val;
      s = db->Get(key, &actual_val);
      if (s.IsNotFound()) {
        missing++;
      } else if (!s.ok()) {
        corrupted++;
      } else if (actual_val != expected_val) {
        corrupted++;
      } else {
        verified++;
      }
    }

    delete db;

    std::cout << "Verification results:" << std::endl;
    std::cout << "  Acknowledged writes: " << acked_keys.size() << std::endl;
    std::cout << "  Successfully recovered: " << verified << std::endl;
    std::cout << "  Missing keys: " << missing << std::endl;
    std::cout << "  Corrupted keys: " << corrupted << std::endl;

    if (missing == 0 && corrupted == 0) {
      std::cout << "PASS: Zero data loss verified after crash." << std::endl;
      return 0;
    } else {
      std::cerr << "FAIL: Data loss detected after crash!" << std::endl;
      return 2;
    }
  }

  std::cerr << "Unknown mode: " << mode << std::endl;
  return 1;
}
