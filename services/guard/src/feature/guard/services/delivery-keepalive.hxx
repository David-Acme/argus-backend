#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

class DeliveryKeepalive
{
public:
  [[nodiscard]] uint64_t hold(std::function<void()> inProgress);
  void release(uint64_t token);
  size_t touch() const;
  [[nodiscard]] size_t held() const;

private:
  mutable std::mutex mutex_;
  std::map<uint64_t, std::function<void()>> held_;
  uint64_t next_{0};
};
