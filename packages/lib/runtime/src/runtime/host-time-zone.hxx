#pragma once

#include <string>
#include <string_view>

[[nodiscard]] std::string zoneNameFromPath(std::string_view path);
[[nodiscard]] std::string hostTimeZone(std::string_view localtimePath = "/etc/localtime");
