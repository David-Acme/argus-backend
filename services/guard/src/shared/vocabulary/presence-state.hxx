#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

enum class PresenceState : uint8_t
{
  Unknown = 0,
  Home,
  Away
};

inline std::string presenceStateToString(PresenceState state)
{
  switch (state) {
    case PresenceState::Home:
      return "home";
    case PresenceState::Away:
      return "away";
    case PresenceState::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline PresenceState presenceStateFromString(std::string_view value)
{
  if (value == "home")
    return PresenceState::Home;
  if (value == "away")
    return PresenceState::Away;
  return PresenceState::Unknown;
}

enum class PresenceSource : uint8_t
{
  None = 0,
  LanSession,
  TunnelSession,
  AppActivity,
  Camera,
  Timeout,
  Consent
};

inline std::string presenceSourceToString(PresenceSource source)
{
  switch (source) {
    case PresenceSource::LanSession:
      return "lan_session";
    case PresenceSource::TunnelSession:
      return "tunnel_session";
    case PresenceSource::AppActivity:
      return "app_activity";
    case PresenceSource::Camera:
      return "camera";
    case PresenceSource::Timeout:
      return "timeout";
    case PresenceSource::Consent:
      return "consent";
    case PresenceSource::None:
      return "none";
  }
  return "none";
}

inline PresenceSource presenceSourceFromString(std::string_view value)
{
  if (value == "lan_session")
    return PresenceSource::LanSession;
  if (value == "tunnel_session")
    return PresenceSource::TunnelSession;
  if (value == "app_activity")
    return PresenceSource::AppActivity;
  if (value == "camera")
    return PresenceSource::Camera;
  if (value == "timeout")
    return PresenceSource::Timeout;
  if (value == "consent")
    return PresenceSource::Consent;
  return PresenceSource::None;
}

struct PresenceRow
{
  int64_t userId{0};
  int64_t environmentId{0};
  PresenceState state{PresenceState::Unknown};
  PresenceSource source{PresenceSource::None};
  int64_t since{0};
  int64_t lastHomeAt{0};
  int64_t lastSignalAt{0};
};

namespace presence
{

inline PresenceState stateOf(std::span<const PresenceRow> rows, int64_t userId)
{
  const auto row = std::ranges::find(rows, userId, &PresenceRow::userId);
  return row == rows.end() ? PresenceState::Unknown : row->state;
}

inline PresenceState overall(std::span<const PresenceRow> rows)
{
  if (rows.empty())
    return PresenceState::Unknown;
  if (std::ranges::any_of(rows, [](const PresenceRow& row) {
        return row.state == PresenceState::Home;
      }))
    return PresenceState::Home;
  if (std::ranges::all_of(rows, [](const PresenceRow& row) {
        return row.state == PresenceState::Away;
      }))
    return PresenceState::Away;
  return PresenceState::Unknown;
}

}
