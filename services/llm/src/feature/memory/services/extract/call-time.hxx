#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct CallTimeInput
{
  std::string_view text;
  std::string_view lang;
  int64_t now{0};
};

struct CallTime
{
  int64_t fireAt{0};
  std::size_t phraseBegin{0};
  std::size_t phraseEnd{0};
};

namespace call_time
{
std::optional<CallTime> resolve(const CallTimeInput& input);

std::string withoutPhrase(std::string_view text, const CallTime& time);
}
