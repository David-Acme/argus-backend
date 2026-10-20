#pragma once

#include <auth/user-role.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace turn
{

struct ScreenedUtterance
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string_view text;
  bool anonymousCamera{false};
};

struct ScreenedText
{
  std::string text;
  bool withheld{false};
};

class UtteranceScreen
{
public:
  virtual ~UtteranceScreen() = default;

  [[nodiscard]] virtual ScreenedText screen(const ScreenedUtterance& utterance) const = 0;
};

}
