#pragma once

#include <string_view>

namespace routes
{

inline constexpr std::string_view kServiceType = "_argus-route._tcp";
inline constexpr std::string_view kTxtPath = "path";
inline constexpr std::string_view kTxtHttps = "https";

}
