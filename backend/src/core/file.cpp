#include "bourse/core/file.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <io.h>
#define BOURSE_OPEN ::_open
#define BOURSE_CLOSE ::_close
#define BOURSE_FSYNC ::_commit
#else
#include <unistd.h>
#define BOURSE_OPEN ::open
#define BOURSE_CLOSE ::close
#define BOURSE_FSYNC ::fsync
#endif

namespace bourse {
namespace {

std::string errnoMessage(const char* what, const std::string& path) {
  std::ostringstream oss;
  oss << what << " '" << path << "': " << std::strerror(errno) << " (errno " << errno << ')';
  return oss.str();
}

}  // namespace

File::~File() { close(); }

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) { other.fd_ = -1; }

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    path_ = std::move(other.path_);
    other.fd_ = -1;
  }
  return *this;
}

Result<File> File::open(const std::string& path, Mode mode) {
  int flags = 0;
  switch (mode) {
    case Mode::kReadOnly: flags = O_RDONLY; break;
    case Mode::kReadWrite: flags = O_RDWR; break;
    case Mode::kCreateReadWrite: flags = O_RDWR | O_CREAT; break;
    case Mode::kTruncateReadWrite: flags = O_RDWR | O_CREAT | O_TRUNC; break;
  }
#if defined(_WIN32)
  flags |= _O_BINARY;
  const int permissions = _S_IREAD | _S_IWRITE;
#else
  flags |= O_CLOEXEC;
  const int permissions = 0644;
#endif

  const int fd = BOURSE_OPEN(path.c_str(), flags, permissions);
  if (fd < 0) {
    return Status::ioError(errnoMessage("open", path));
  }
  return File(fd, path);
}

void File::close() noexcept {
  if (fd_ >= 0) {
    BOURSE_CLOSE(fd_);
    fd_ = -1;
  }
}

Status File::readAt(std::uint64_t offset, void* buffer, std::size_t n) const {
  if (fd_ < 0) {
    return Status::ioError("readAt on a closed file");
  }
  auto* out = static_cast<char*>(buffer);
  std::size_t done = 0;
  while (done < n) {
#if defined(_WIN32)
    if (::_lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET) < 0) {
      return Status::ioError(errnoMessage("lseek", path_));
    }
    const int got = ::_read(fd_, out + done, static_cast<unsigned int>(n - done));
#else
    const ssize_t got = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
#endif
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::ioError(errnoMessage("read", path_));
    }
    if (got == 0) {
      std::ostringstream oss;
      oss << "short read on '" << path_ << "': wanted " << n << " bytes at offset " << offset << ", got " << done;
      return Status::ioError(oss.str());
    }
    done += static_cast<std::size_t>(got);
  }
  return Status::success();
}

Status File::writeAt(std::uint64_t offset, const void* buffer, std::size_t n) {
  if (fd_ < 0) {
    return Status::ioError("writeAt on a closed file");
  }
  const auto* in = static_cast<const char*>(buffer);
  std::size_t done = 0;
  while (done < n) {
#if defined(_WIN32)
    if (::_lseeki64(fd_, static_cast<__int64>(offset + done), SEEK_SET) < 0) {
      return Status::ioError(errnoMessage("lseek", path_));
    }
    const int put = ::_write(fd_, in + done, static_cast<unsigned int>(n - done));
#else
    const ssize_t put = ::pwrite(fd_, in + done, n - done, static_cast<off_t>(offset + done));
#endif
    if (put < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::ioError(errnoMessage("write", path_));
    }
    done += static_cast<std::size_t>(put);
  }
  return Status::success();
}

Result<std::uint64_t> File::append(const void* buffer, std::size_t n) {
  Result<std::uint64_t> current = size();
  if (!current.ok()) {
    return current.status();
  }
  const std::uint64_t offset = current.value();
  Status status = writeAt(offset, buffer, n);
  if (!status.ok()) {
    return status;
  }
  return offset;
}

Result<std::uint64_t> File::size() const {
  if (fd_ < 0) {
    return Status::ioError("size on a closed file");
  }
#if defined(_WIN32)
  struct _stat64 info {};
  if (::_fstat64(fd_, &info) != 0) {
    return Status::ioError(errnoMessage("fstat", path_));
  }
#else
  struct stat info {};
  if (::fstat(fd_, &info) != 0) {
    return Status::ioError(errnoMessage("fstat", path_));
  }
#endif
  return static_cast<std::uint64_t>(info.st_size);
}

Status File::truncate(std::uint64_t new_size) {
  if (fd_ < 0) {
    return Status::ioError("truncate on a closed file");
  }
#if defined(_WIN32)
  if (::_chsize_s(fd_, static_cast<__int64>(new_size)) != 0) {
    return Status::ioError(errnoMessage("chsize", path_));
  }
#else
  if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0) {
    return Status::ioError(errnoMessage("ftruncate", path_));
  }
#endif
  return Status::success();
}

Status File::sync() {
  if (fd_ < 0) {
    return Status::ioError("sync on a closed file");
  }
  if (BOURSE_FSYNC(fd_) != 0) {
    return Status::ioError(errnoMessage("fsync", path_));
  }
  return Status::success();
}

bool File::exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

Status File::removeFile(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Status::ioError("remove '" + path + "': " + ec.message());
  }
  return Status::success();
}

Status File::rename(const std::string& from, const std::string& to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    return Status::ioError("rename '" + from + "' -> '" + to + "': " + ec.message());
  }
  return Status::success();
}

Status File::ensureDirectory(const std::string& path) {
  if (path.empty()) {
    return Status::success();
  }
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return Status::ioError("create_directories '" + path + "': " + ec.message());
  }
  return Status::success();
}

Result<std::string> File::readWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::ioError("cannot open '" + path + "' for reading");
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

Status File::writeWholeFile(const std::string& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return Status::ioError("cannot open '" + path + "' for writing");
  }
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!out) {
    return Status::ioError("short write to '" + path + "'");
  }
  return Status::success();
}

}  // namespace bourse
