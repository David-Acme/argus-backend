#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace guard_test
{

inline int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

inline std::string tempPath(const std::string& name)
{
  return (std::filesystem::temp_directory_path() / name).string();
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(tempPath(std::string(stem) + "-" + std::to_string(::getpid()) +
                       "-" + std::to_string(tempCounter()) + ".db"))
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

}
