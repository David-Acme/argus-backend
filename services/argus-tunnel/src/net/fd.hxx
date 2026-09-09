#pragma once

#include <unistd.h>

#include <utility>

// Owning file descriptor; closes on destruction.
class UniqueFd
{
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_{fd} {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  int release() { return std::exchange(fd_, -1); }
  void reset()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_{-1};
};
