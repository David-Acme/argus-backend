#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace live_broker
{

inline constexpr const char* kUrlVariable = "ARGUS_NATS_URL";
inline constexpr const char* kMissingUrl =
    "ARGUS_NATS_URL must name a JetStream broker when CI=true";

inline std::string url()
{
  const char* raw = std::getenv(kUrlVariable);
  return raw == nullptr ? std::string{} : std::string(raw);
}

inline bool required()
{
  const char* ci = std::getenv("CI");
  return ci != nullptr && std::string_view(ci) == "true";
}

inline bool skipped()
{
  return url().empty() && !required();
}

}
