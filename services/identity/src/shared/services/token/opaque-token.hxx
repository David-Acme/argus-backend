#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace opaque_token
{

[[nodiscard]] std::optional<std::string> mint();

[[nodiscard]] std::optional<std::string> sha256Hex(std::string_view token);

}
