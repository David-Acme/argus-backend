#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <shared/vocabulary/privacy-choices.hxx>

struct UserPrivacySchema
{
  int64_t userId{0};
  int64_t noticeVersion{0};
  PrivacyChoices choices;
  int64_t decidedAt{0};
  int64_t updatedAt{0};

  UserPrivacySchema() = default;
  explicit UserPrivacySchema(const drogon::orm::Row& row);
};

struct HouseholdPrivacySchema
{
  PrivacyChoices allowed{.presence = true,
                         .faceCameras = true,
                         .voiceLearning = true,
                         .cameraAudio = true};
  bool visitorRecognition{false};
  std::optional<int64_t> visitorAcknowledgedAt;
  std::optional<int64_t> visitorAcknowledgedBy;
  std::optional<int64_t> updatedBy;
  std::optional<int64_t> updatedAt;

  HouseholdPrivacySchema() = default;
  explicit HouseholdPrivacySchema(const drogon::orm::Row& row);
};
