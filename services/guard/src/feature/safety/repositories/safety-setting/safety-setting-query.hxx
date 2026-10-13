#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace safety_setting_query
{

inline constexpr std::string_view SELECT_SETTING =
    "SELECT duress_enabled, updated_at, updated_by, duress_phrase_key FROM "
    "guard_safety_setting WHERE id = 1";

inline constexpr std::string_view UPSERT_SETTING =
    "INSERT INTO guard_safety_setting (id, duress_enabled, updated_at, "
    "updated_by) VALUES (1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
    "duress_enabled = excluded.duress_enabled, "
    "updated_at = excluded.updated_at, updated_by = excluded.updated_by";

inline constexpr std::string_view UPSERT_PHRASE =
    "INSERT INTO guard_safety_setting (id, duress_phrase_key, updated_at, "
    "updated_by) VALUES (1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
    "duress_phrase_key = excluded.duress_phrase_key, "
    "updated_at = excluded.updated_at, updated_by = excluded.updated_by";

}

struct SafetySetting
{
  bool duressEnabled{false};
  int64_t updatedAt{0};
  int64_t updatedBy{0};
  std::string phraseKey;
};

struct SafetySettingUpdateInput
{
  bool duressEnabled{false};
  int64_t updatedBy{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};

struct SafetyPhraseUpdateInput
{
  std::string phraseKey;
  int64_t updatedBy{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};
