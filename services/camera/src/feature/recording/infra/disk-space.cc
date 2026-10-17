#include "disk-space.hxx"

#include <sys/statvfs.h>

namespace disk_space
{

using Statvfs = struct statvfs;

int64_t freeBytesOf(const std::string& path)
{
  Statvfs info{};
  if (::statvfs(path.c_str(), &info) != 0)
    return -1;
  return static_cast<int64_t>(info.f_bavail) * static_cast<int64_t>(info.f_frsize);
}

}
