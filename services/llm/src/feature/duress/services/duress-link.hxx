#pragma once

#include <auth/user-role.hxx>

#include <cstdint>
#include <optional>
#include <string>

enum class DuressDelivery : uint8_t
{
  Delivered,
  Retry,
  Dropped
};

struct DuressRaise
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string key;
};

class DuressLink
{
public:
  virtual ~DuressLink() = default;

  [[nodiscard]] virtual std::optional<std::string> phraseKey() const = 0;

  [[nodiscard]] virtual DuressDelivery raise(const DuressRaise& raise) const = 0;
};
