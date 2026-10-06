#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>

namespace argus::mcp
{

struct ConfirmationKey
{
  int64_t userId{0};
  std::string tool;
  std::string target;

  [[nodiscard]] bool operator<(const ConfirmationKey& other) const;
};

struct ConfirmationLimits
{
  std::chrono::seconds lifetime{120};
  std::size_t capacity{256};
};

class ConfirmationLedger
{
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit ConfirmationLedger(ConfirmationLimits limits = {}, Clock clock = {});

  [[nodiscard]] std::string issue(const ConfirmationKey& key);

  [[nodiscard]] bool consume(const ConfirmationKey& key, std::string_view token);

  [[nodiscard]] std::size_t pending() const;

private:
  struct Entry
  {
    std::string token;
    std::chrono::steady_clock::time_point expiresAt;
  };

  void sweep(std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::chrono::steady_clock::time_point now() const;

  ConfirmationLimits limits_;
  Clock clock_;
  mutable std::mutex mutex_;
  std::mt19937_64 random_;
  std::map<ConfirmationKey, Entry> entries_;
};

}
