#pragma once

#include <cstdint>
#include <string>

namespace disk_space
{
[[nodiscard]] int64_t freeBytesOf(const std::string& path);
}
