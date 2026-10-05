#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

struct DisarmRequest
{
  int64_t userId{0};
  std::string userName;
  std::optional<std::string> pin;
  std::optional<int64_t> environmentId;
};

enum class DisarmVerdict : uint8_t
{
  Allowed = 0,
  Duress
};

class DisarmGate
{
public:
  virtual ~DisarmGate() = default;

  [[nodiscard]] virtual drogon::Task<DisarmVerdict>
  authorize(const DisarmRequest& request) const = 0;

  virtual void duress(const DisarmRequest& request) const = 0;
};
