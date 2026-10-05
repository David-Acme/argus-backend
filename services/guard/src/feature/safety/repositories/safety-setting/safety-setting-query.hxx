#pragma once

#include <cstdint>
#include <string_view>

namespace safety_setting_query
{

inline constexpr std::string_view SELECT_SETTING =
    "SELECT duress_enabled, updated_at, updated_by FROM guard_safety_setting "
    "WHERE id = 1";

inline constexpr std::string_view UPSERT_SETTING =
    "INSERT INTO guard_safety_setting (id, duress_enabled, updated_at, "
    "updated_by) VALUES (1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
    "duress_enabled = excluded.duress_enabled, "
    "updated_at = excluded.updated_at, updated_by = excluded.updated_by";

}

struct SafetySetting
{
  bool duressEnabled{false};
  int64_t updatedAt{0};
  int64_t updatedBy{0};
};

struct SafetySettingUpdateInput
{
  bool duressEnabled{false};
  int64_t updatedBy{0};
  int64_t now{0};
};
