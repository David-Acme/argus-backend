#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>

namespace tapo_recordings
{

[[nodiscard]] std::optional<std::string> deviceDate(std::string_view isoDate);

[[nodiscard]] Json::Value days(const Json::Value& searchResults);

[[nodiscard]] Json::Value segments(const Json::Value& videoResults);

}
