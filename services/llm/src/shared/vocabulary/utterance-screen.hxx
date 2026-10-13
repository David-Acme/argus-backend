#pragma once

#include <auth/user-role.hxx>

#include <cstdint>
#include <string_view>

namespace turn
{

struct ScreenedUtterance
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string_view text;
};

class UtteranceScreen
{
public:
  virtual ~UtteranceScreen() = default;

  virtual void screen(const ScreenedUtterance& utterance) const = 0;
};

}
