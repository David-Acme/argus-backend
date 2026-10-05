#pragma once

#include <cstdint>
#include <json/value.h>
#include <vector>

namespace visitor_dto
{
[[nodiscard]] std::vector<int64_t> idsOf(const Json::Value& array);
}
