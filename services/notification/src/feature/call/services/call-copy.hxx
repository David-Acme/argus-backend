#pragma once

#include <feature/call/vocabulary/call-trigger.hxx>
#include <json/value.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct CallCopyInput
{
  CallTrigger trigger{CallTrigger::Assistant};
  std::string lang;
  const Json::Value& data;
  std::string userName;
  int64_t now{0};
};

struct CallCopy
{
  std::string title;
  std::string summary;
  std::string openingLine;
  std::string missedTitle;
  std::string missedLine;
  std::string followupLine;
};

namespace call_copy
{
std::string_view normalizeLang(std::string_view lang);

std::string clockTime(int64_t epochSeconds);

CallCopy render(const CallCopyInput& input);

std::string joinFollowups(const std::string& opening,
                          const std::vector<std::string>& followups);
}
