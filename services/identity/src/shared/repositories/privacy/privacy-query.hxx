#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <shared/vocabulary/privacy-choices.hxx>
#include <string_view>

namespace privacy_query
{

inline constexpr std::string_view FIND_USER =
    "SELECT * FROM user_privacy WHERE user_id = ?";

inline constexpr std::string_view FIND_ALL_USERS =
    "SELECT p.* FROM user_privacy p JOIN user u ON u.id = p.user_id "
    "WHERE u.deleted_at IS NULL";

inline constexpr std::string_view UPSERT_USER =
    "INSERT INTO user_privacy (user_id, notice_version, presence, "
    "face_cameras, voice_learning, camera_audio) VALUES (?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(user_id) DO UPDATE SET "
    "notice_version = excluded.notice_version, presence = excluded.presence, "
    "face_cameras = excluded.face_cameras, "
    "voice_learning = excluded.voice_learning, "
    "camera_audio = excluded.camera_audio, "
    "updated_at = strftime('%s', 'now')";

inline constexpr std::string_view ENSURE_HOUSEHOLD =
    "INSERT OR IGNORE INTO household_privacy (id) VALUES (1)";

inline constexpr std::string_view FIND_HOUSEHOLD =
    "SELECT * FROM household_privacy WHERE id = 1";

inline constexpr std::string_view UPDATE_HOUSEHOLD_PREFIX =
    "UPDATE household_privacy SET ";
inline constexpr std::string_view UPDATE_COL_PRESENCE = "presence = ?";
inline constexpr std::string_view UPDATE_COL_FACE_CAMERAS = "face_cameras = ?";
inline constexpr std::string_view UPDATE_COL_VOICE_LEARNING =
    "voice_learning = ?";
inline constexpr std::string_view UPDATE_COL_CAMERA_AUDIO = "camera_audio = ?";
inline constexpr std::string_view UPDATE_COL_VISITOR_RECOGNITION = "visitor_recognition = ?";
inline constexpr std::string_view UPDATE_COL_VISITOR_ACK =
    "visitor_ack_at = strftime('%s', 'now'), "
    "visitor_ack_by = ?, visitor_ack_version = ?";
inline constexpr std::string_view UPDATE_HOUSEHOLD_SUFFIX =
    ", updated_by = ?, updated_at = strftime('%s', 'now') WHERE id = 1";

}

struct UserPrivacyUpsertInput
{
  int64_t userId{0};
  int64_t noticeVersion{0};
  PrivacyChoices choices;
  drogon::orm::DbClient* client{nullptr};
};

struct HouseholdPrivacyUpdateInput
{
  std::optional<bool> presence;
  std::optional<bool> faceCameras;
  std::optional<bool> voiceLearning;
  std::optional<bool> cameraAudio;
  std::optional<bool> visitorRecognition;
  int64_t updatedBy{0};
  drogon::orm::DbClient* client{nullptr};
};
