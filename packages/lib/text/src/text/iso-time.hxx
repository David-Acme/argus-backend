#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace iso_time
{

[[nodiscard]] std::optional<int64_t> parse(std::string_view text);

[[nodiscard]] std::string format(int64_t epoch);

}
