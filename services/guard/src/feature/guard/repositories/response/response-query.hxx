#pragma once

#include <cstdint>
#include <feature/guard/vocabulary/recipient-mode.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace response_query
{

inline constexpr std::string_view LIST_RECIPIENTS =
    "SELECT r.environment_id, r.user_id, r.mode, r.step, r.on_duty "
    "FROM guard_response_recipient r JOIN guard_environment e "
    "ON e.id = r.environment_id WHERE r.environment_id = ? "
    "ORDER BY r.user_id ASC";

inline constexpr std::string_view LIST_CONTACTS =
    "SELECT c.id, c.environment_id, c.position, c.name, c.phone, c.note "
    "FROM guard_response_contact c JOIN guard_environment e "
    "ON e.id = c.environment_id WHERE c.environment_id = ? "
    "ORDER BY c.position ASC, c.id ASC";

inline constexpr std::string_view FIND_SETTING =
    "SELECT s.environment_id, s.emergency_number, s.step_seconds "
    "FROM guard_response_setting s JOIN guard_environment e "
    "ON e.id = s.environment_id WHERE s.environment_id = ?";

inline constexpr std::string_view DELETE_RECIPIENTS =
    "DELETE FROM guard_response_recipient WHERE environment_id = ?";

inline constexpr std::string_view DELETE_CONTACTS =
    "DELETE FROM guard_response_contact WHERE environment_id = ?";

inline constexpr std::string_view INSERT_RECIPIENT_HEAD =
    "INSERT INTO guard_response_recipient (environment_id, user_id, mode, "
    "step, "
    "on_duty, updated_at) VALUES ";

inline constexpr std::string_view INSERT_RECIPIENT_ROW = "(?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view INSERT_CONTACT_HEAD =
    "INSERT INTO guard_response_contact (environment_id, position, name, "
    "phone, "
    "note, updated_at) VALUES ";

inline constexpr std::string_view INSERT_CONTACT_ROW = "(?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPSERT_SETTING =
    "INSERT INTO guard_response_setting (environment_id, emergency_number, "
    "step_seconds, updated_at) VALUES (?, ?, ?, ?) "
    "ON CONFLICT (environment_id) DO UPDATE SET "
    "emergency_number = excluded.emergency_number, "
    "step_seconds = excluded.step_seconds, updated_at = excluded.updated_at";

inline constexpr std::string_view UPSERT_DUTY =
    "INSERT INTO guard_response_recipient (environment_id, user_id, mode, "
    "step, "
    "on_duty, updated_at) VALUES (?, ?, NULL, NULL, ?, ?) "
    "ON CONFLICT (environment_id, user_id) DO UPDATE SET "
    "on_duty = excluded.on_duty, updated_at = excluded.updated_at";

}

struct ResponseRecipientRow
{
  int64_t environmentId{0};
  int64_t userId{0};
  std::optional<RecipientMode> mode;
  std::optional<int> step;
  bool onDuty{false};
};

struct ResponseContactRow
{
  int64_t id{0};
  int64_t environmentId{0};
  int position{0};
  std::string name;
  std::string phone;
  std::string note;
};

struct ResponseSettingRow
{
  int64_t environmentId{0};
  std::string emergencyNumber;
  int stepSeconds{45};
};

struct ResponseConfigRows
{
  std::vector<ResponseRecipientRow> recipients;
  std::vector<ResponseContactRow> contacts;
  ResponseSettingRow setting;
};

struct ResponseRecipientInput
{
  int64_t userId{0};
  RecipientMode mode{RecipientMode::Call};
  int step{0};
  bool onDuty{false};
};

struct ResponseContactInput
{
  std::string name;
  std::string phone;
  std::string note;
};

struct ResponseReplaceInput
{
  int64_t environmentId{0};
  std::string emergencyNumber;
  int stepSeconds{45};
  std::vector<ResponseRecipientInput> recipients;
  std::vector<ResponseContactInput> contacts;
  int64_t at{0};
};

struct ResponseDutyInput
{
  int64_t environmentId{0};
  int64_t userId{0};
  bool onDuty{false};
  int64_t at{0};
};
