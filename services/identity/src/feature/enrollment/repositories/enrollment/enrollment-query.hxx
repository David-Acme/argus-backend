#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace enrollment_query
{

inline constexpr std::string_view COUNT_USERS =
    "SELECT COUNT(*) AS total FROM user WHERE deleted_at IS NULL";

inline constexpr std::string_view INSERT_USER =
    "INSERT INTO user (name, last_name, role, lang, is_active) "
    "VALUES (?, ?, ?, ?, 1)";

inline constexpr std::string_view INSERT_PERSON =
    "INSERT INTO person (user_id, name, alias, observation, first_seen_at, "
    "last_seen_at) VALUES (?, ?, '', '', strftime('%s', 'now'), "
    "strftime('%s', 'now'))";

inline constexpr std::string_view INSERT_FACE_EMBEDDING =
    "INSERT INTO face_embedding (person_id, embedding, angle_label, quality, "
    "model) VALUES (?, ?, 'frontal', ?, ?)";

inline constexpr std::string_view CONSUME_INVITATION =
    "UPDATE user_invitation SET redemption_count = redemption_count + 1, "
    "updated_at = strftime('%s', 'now') WHERE token_hash = ? "
    "AND revoked_at IS NULL AND expires_at > ? "
    "AND redemption_count < max_redemptions";

inline constexpr std::string_view INSERT_REDEMPTION =
    "INSERT INTO invitation_redemption (invitation_id, user_id) VALUES (?, ?)";

}

struct EnrollmentUserInput
{
  std::string name;
  std::string lastName;
  std::string role;
  std::string lang;
  drogon::orm::DbClient* client{nullptr};
};

struct EnrollmentPersonInput
{
  int64_t userId{0};
  std::string name;
  drogon::orm::DbClient* client{nullptr};
};

struct EnrollmentFaceInput
{
  int64_t personId{0};
  std::string embedding;
  float quality{0.0F};
  drogon::orm::DbClient* client{nullptr};
};

struct EnrollmentInvitationConsumeInput
{
  std::string tokenHash;
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};

struct EnrollmentRedemptionInput
{
  int64_t invitationId{0};
  int64_t userId{0};
  drogon::orm::DbClient* client{nullptr};
};
