#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MaintenanceSource : uint8_t
{
  App = 0,
  Assistant,
  Reply,
  Alert
};

inline std::string maintenanceSourceToString(MaintenanceSource source)
{
  switch (source) {
    case MaintenanceSource::App:
      return "app";
    case MaintenanceSource::Assistant:
      return "assistant";
    case MaintenanceSource::Reply:
      return "reply";
    case MaintenanceSource::Alert:
      return "alert";
  }
  return "app";
}

inline std::optional<MaintenanceSource> maintenanceSourceFromString(std::string_view value)
{
  if (value == "app")
    return MaintenanceSource::App;
  if (value == "assistant")
    return MaintenanceSource::Assistant;
  if (value == "reply")
    return MaintenanceSource::Reply;
  if (value == "alert")
    return MaintenanceSource::Alert;
  return std::nullopt;
}
