#pragma once

#include <cstdint>
#include <string_view>

namespace visitor_setting_query
{
inline constexpr std::string_view FIND =
    "SELECT unnamed_retention_days, updated_by, updated_at FROM visitor_setting "
    "WHERE id = 1";

inline constexpr std::string_view UPDATE_RETENTION =
    "INSERT INTO visitor_setting (id, unnamed_retention_days, updated_by, "
    "updated_at) VALUES (1, ?, ?, strftime('%s', 'now')) "
    "ON CONFLICT (id) DO UPDATE SET unnamed_retention_days = "
    "excluded.unnamed_retention_days, updated_by = excluded.updated_by, "
    "updated_at = excluded.updated_at";
}

inline constexpr int64_t kDefaultUnnamedRetentionDays = 30;
inline constexpr int64_t kMinUnnamedRetentionDays = 1;
inline constexpr int64_t kMaxUnnamedRetentionDays = 60;

struct VisitorSetting
{
  int64_t unnamedRetentionDays{kDefaultUnnamedRetentionDays};
  int64_t updatedBy{0};
  int64_t updatedAt{0};
};

struct VisitorSettingUpdateInput
{
  int64_t unnamedRetentionDays{kDefaultUnnamedRetentionDays};
  int64_t updatedBy{0};
};
