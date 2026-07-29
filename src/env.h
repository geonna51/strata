#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
#include "filename.h"
#include "strata/status.h"

namespace strata {

// Forces physical disk platter/NAND flush on a file descriptor
inline Status FsyncFile(int fd) {
  if (fd < 0) return Status::IOError("Invalid fd");
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) < 0) {
    if (::fsync(fd) < 0) {
      return Status::IOError("fsync failed: " + std::string(strerror(errno)));
    }
  }
#else
  if (::fdatasync(fd) < 0) {
    return Status::IOError("fdatasync failed: " + std::string(strerror(errno)));
  }
#endif
  return Status::OK();
}

// Synchronizes directory metadata to ensure directory entries (link, unlink, rename)
// are committed to physical storage across unexpected power loss.
inline Status SyncDirectory(const std::string& dirpath) {
  int fd = ::open(dirpath.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::IOError("Failed to open directory for sync: " + dirpath +
                           " (" + strerror(errno) + ")");
  }
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) < 0) {
    ::fsync(fd);
  }
#else
  ::fsync(fd);
#endif
  ::close(fd);
  return Status::OK();
}

// Atomically writes the CURRENT pointer file and syncs both the file and parent directory.
inline Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number) {
  char manifest_name[64];
  std::snprintf(manifest_name, sizeof(manifest_name), "MANIFEST-%06llu\n",
                static_cast<unsigned long long>(manifest_number));

  std::string tmp_file = CurrentFileName(dbname) + ".tmp";
  int fd = ::open(tmp_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    return Status::IOError("Failed to create temporary CURRENT file: " + tmp_file +
                           " (" + strerror(errno) + ")");
  }

  size_t len = strlen(manifest_name);
  ssize_t written = ::write(fd, manifest_name, len);
  if (written != static_cast<ssize_t>(len)) {
    ::close(fd);
    return Status::IOError("Failed to write to temporary CURRENT file");
  }

  Status s = FsyncFile(fd);
  ::close(fd);
  if (!s.ok()) {
    return s;
  }

  if (::rename(tmp_file.c_str(), CurrentFileName(dbname).c_str()) != 0) {
    return Status::IOError("Failed to rename temporary CURRENT file: " +
                           std::string(strerror(errno)));
  }

  // Sync parent directory so the rename is durable across power loss
  return SyncDirectory(dbname);
}

}  // namespace strata
