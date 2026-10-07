#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace reminder_readback
{

struct Spoken
{
  int64_t fireAt{0};
  int64_t now{0};
  std::string_view lang;
  bool called{false};
};

[[nodiscard]] std::string sentence(const Spoken& spoken);

}
