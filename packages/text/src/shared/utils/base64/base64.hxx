#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace base64
{

std::string encode(std::string_view bytes);

std::optional<std::string> decode(std::string_view text);

} // namespace base64
