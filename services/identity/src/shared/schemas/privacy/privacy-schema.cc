#include "privacy-schema.hxx"

#include <drogon/orm/Field.h>

namespace
{
PrivacyChoices choicesOf(const drogon::orm::Row& row)
{
  return {.presence = row["presence"].as<int64_t>() != 0,
          .faceCameras = row["face_cameras"].as<int64_t>() != 0,
          .voiceLearning = row["voice_learning"].as<int64_t>() != 0,
          .cameraAudio = row["camera_audio"].as<int64_t>() != 0};
}
}

UserPrivacySchema::UserPrivacySchema(const drogon::orm::Row& row)
    : userId(row["user_id"].as<int64_t>()),
      noticeVersion(row["notice_version"].as<int64_t>()),
      choices(choicesOf(row)),
      decidedAt(row["decided_at"].as<int64_t>()),
      updatedAt(row["updated_at"].as<int64_t>())
{
}

HouseholdPrivacySchema::HouseholdPrivacySchema(const drogon::orm::Row& row)
    : allowed(choicesOf(row)), visitorRecognition(row["visitor_recognition"].as<int64_t>() != 0)
{
  if (!row["visitor_ack_at"].isNull())
    visitorAcknowledgedAt = row["visitor_ack_at"].as<int64_t>();
  if (!row["visitor_ack_by"].isNull())
    visitorAcknowledgedBy = row["visitor_ack_by"].as<int64_t>();
  if (!row["updated_by"].isNull())
    updatedBy = row["updated_by"].as<int64_t>();
  if (!row["updated_at"].isNull())
    updatedAt = row["updated_at"].as<int64_t>();
}
