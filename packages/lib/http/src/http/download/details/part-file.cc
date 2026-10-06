#include "part-file.hxx"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace file_download::details
{

PartFile::PartFile(PartFile&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1))
{
}

PartFile& PartFile::operator=(PartFile&& other) noexcept
{
  if (this != &other)
  {
    close();
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}

PartFile::~PartFile() { close(); }

void PartFile::close()
{
  if (descriptor_ >= 0)
    ::close(std::exchange(descriptor_, -1));
}

PartOpen PartFile::open(const std::filesystem::path& path)
{
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_APPEND, 0644);
  if (descriptor < 0)
    return {.file = PartFile{}, .busy = false, .error = lastSystemError()};
  PartFile file(descriptor);
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
  {
    const bool busy = errno == EWOULDBLOCK;
    return {.file = PartFile{}, .busy = busy, .error = lastSystemError()};
  }
  return {.file = std::move(file), .busy = false, .error = {}};
}

std::optional<std::uint64_t> PartFile::size() const
{
  struct stat status{};
  if (::fstat(descriptor_, &status) != 0)
    return std::nullopt;
  return static_cast<std::uint64_t>(status.st_size);
}

bool PartFile::truncate(std::uint64_t length) const
{
  return ::ftruncate(descriptor_, static_cast<off_t>(length)) == 0;
}

bool PartFile::append(std::span<const char> bytes) const
{
  while (!bytes.empty())
  {
    const auto written = ::write(descriptor_, bytes.data(), bytes.size());
    if (written < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
  return true;
}

bool PartFile::sync() const { return ::fsync(descriptor_) == 0; }

std::string lastSystemError() { return std::error_code(errno, std::system_category()).message(); }

bool syncDirectory(const std::filesystem::path& directory)
{
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0)
    return false;
  const bool synced = ::fsync(descriptor) == 0;
  ::close(descriptor);
  return synced;
}

}
