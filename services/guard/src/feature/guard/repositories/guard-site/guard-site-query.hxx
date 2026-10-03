#pragma once

#include <feature/guard/vocabulary/site-profile.hxx>
#include <shared/vocabulary/guard-mode.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace guard_site_query
{

inline constexpr std::string_view SELECT_SITE =
    "SELECT profile, schedule_enabled, asleep_hours, open_hours, "
    "staffed_hours, closed_mode, digest_hour, updated_at FROM guard_site "
    "WHERE id = 1";

inline constexpr std::string_view SEED_SITE =
    "INSERT OR IGNORE INTO guard_site (id, profile, schedule_enabled, "
    "asleep_hours, open_hours, staffed_hours, closed_mode, digest_hour, "
    "updated_at) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE_SITE_PREFIX =
    "UPDATE guard_site SET updated_at = ?";

inline constexpr std::string_view UPDATE_COL_PROFILE = ", profile = ?";
inline constexpr std::string_view UPDATE_COL_SCHEDULE_ENABLED =
    ", schedule_enabled = ?";
inline constexpr std::string_view UPDATE_COL_ASLEEP = ", asleep_hours = ?";
inline constexpr std::string_view UPDATE_COL_OPEN = ", open_hours = ?";
inline constexpr std::string_view UPDATE_COL_STAFFED = ", staffed_hours = ?";
inline constexpr std::string_view UPDATE_COL_CLOSED_MODE = ", closed_mode = ?";
inline constexpr std::string_view UPDATE_COL_DIGEST_HOUR = ", digest_hour = ?";

inline constexpr std::string_view UPDATE_SITE_SUFFIX = " WHERE id = 1";

}

struct GuardSite
{
  SiteProfile profile{SiteProfile::Home};
  bool scheduleEnabled{false};
  std::string asleep;
  std::string open;
  std::string staffed;
  GuardMode closedMode{GuardMode::Away};
  int digestHour{21};
  int64_t updatedAt{0};
};

struct GuardSiteUpdateInput
{
  GuardSite seed;
  std::optional<SiteProfile> profile;
  std::optional<bool> scheduleEnabled;
  std::optional<std::string> asleep;
  std::optional<std::string> open;
  std::optional<std::string> staffed;
  std::optional<GuardMode> closedMode;
  std::optional<int> digestHour;
  int64_t updatedAt{0};
};
