#include "tapo-lockout.hxx"

#include <array>
#include <charconv>
#include <cstdlib>
#include <system_error>
#include <string>

namespace
{
std::optional<int> secondsOf(const Json::Value& node)
{
  if (!node.isObject() || !node.isMember("sec_left"))
    return std::nullopt;
  const Json::Value& value = node["sec_left"];
  if (value.isIntegral())
    return value.asInt();
  if (value.isString()) {
    const std::string text = value.asString();
    if (!text.empty() && text.find_first_not_of("0123456789") == std::string::npos) {
      const char* begin = text.data();
      const char* end = begin + text.size();
      int seconds = 0;
      const auto [parsed, error] = std::from_chars(begin, end, seconds);
      if (error == std::errc{} && parsed == end)
        return seconds;
    }
  }
  return std::nullopt;
}

int codeOf(const Json::Value& response, const Json::Value& node)
{
  if (node.isObject() && node.isMember("code") && node["code"].isIntegral())
    return node["code"].asInt();
  if (response.isObject() && response.isMember("error_code") && response["error_code"].isIntegral())
    return response["error_code"].asInt();
  return 0;
}
}

std::optional<tapo_lockout::Reading> tapo_lockout::of(const Json::Value& response)
{
  if (!response.isObject())
    return std::nullopt;
  const std::array<const Json::Value*, 4> nodes{&response["result"]["data"], &response["data"],
                                                &response["result"], &response};
  for (const Json::Value* node : nodes) {
    const auto seconds = secondsOf(*node);
    if (seconds && *seconds > 0)
      return Reading{.secLeft = *seconds, .code = codeOf(response, *node)};
  }
  for (const Json::Value* node : nodes) {
    if (codeOf(response, *node) == kLockedOutCode)
      return Reading{.secLeft = kUnknownLockoutSeconds, .code = kLockedOutCode};
  }
  return std::nullopt;
}
