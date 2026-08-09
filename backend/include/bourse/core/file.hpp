#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bourse/core/result.hpp"

namespace bourse {

/// RAII wrapper over an OS file descriptor offering positional I/O.
///
/// Deliberately built on raw descriptors rather than std::fstream or FILE*:
///
///   * `pread`/`pwrite` do not touch the shared file offset, so the buffer pool
///     can issue page reads from several threads against one File without any
///     locking. An fstream would need a mutex around every seek+read pair.
///   * `sync()` maps to fsync/FlushFileBuffers, which is the only thing that
///     actually makes the write-ahead log durable. Neither fstream nor FILE*
///     exposes it.
///
/// Move-only. The destructor closes; there is no path in the codebase where a
/// descriptor can leak on an early return.
class File {
 public:
  enum class Mode {
    kReadOnly,           ///< must already exist
    kReadWrite,          ///< must already exist
    kCreateReadWrite,    ///< create if missing, keep existing contents
    kTruncateReadWrite,  ///< create if missing, truncate if present
  };

  File() = default;
  ~File();

  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  static Result<File> open(const std::string& path, Mode mode);

  [[nodiscard]] bool isOpen() const noexcept { return fd_ >= 0; }

  [[nodiscard]] int fd() const noexcept { return fd_; }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  /// Reads exactly `n` bytes, retrying short reads. A read that hits EOF early
  /// returns kIoError -- callers always know how much they expect.
  Status readAt(std::uint64_t offset, void* buffer, std::size_t n) const;

  /// Writes exactly `n` bytes, retrying short writes.
  Status writeAt(std::uint64_t offset, const void* buffer, std::size_t n);

  /// Appends at the current end of file and returns the offset written to.
  Result<std::uint64_t> append(const void* buffer, std::size_t n);

  [[nodiscard]] Result<std::uint64_t> size() const;
  Status truncate(std::uint64_t new_size);

  /// Forces data to stable storage. This is the fsync that bounds WAL commit
  /// throughput; see docs/architecture.md for the durability discussion.
  Status sync();

  void close() noexcept;

  // -- filesystem helpers -------------------------------------------------
  [[nodiscard]] static bool exists(const std::string& path);
  static Status removeFile(const std::string& path);
  static Status rename(const std::string& from, const std::string& to);
  static Status ensureDirectory(const std::string& path);
  static Result<std::string> readWholeFile(const std::string& path);
  static Status writeWholeFile(const std::string& path, std::string_view contents);

 private:
  File(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

  int fd_ = -1;
  std::string path_;
};

}  // namespace bourse
