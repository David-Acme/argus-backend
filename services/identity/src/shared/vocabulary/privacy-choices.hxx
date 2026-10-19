#pragma once

#include <cstdint>
#include <json/value.h>

inline constexpr int64_t kPrivacyNoticeVersion = 2;

struct PrivacyChoices
{
  bool presence{false};
  bool faceCameras{false};
  bool voiceLearning{false};
  bool cameraAudio{false};
  bool location{false};

  [[nodiscard]] bool operator==(const PrivacyChoices&) const = default;

  [[nodiscard]] PrivacyChoices both(const PrivacyChoices& other) const
  {
    return {.presence = presence && other.presence,
            .faceCameras = faceCameras && other.faceCameras,
            .voiceLearning = voiceLearning && other.voiceLearning,
            .cameraAudio = cameraAudio && other.cameraAudio,
            .location = location && other.location};
  }

  [[nodiscard]] Json::Value toJson() const
  {
    Json::Value json(Json::objectValue);
    json["presence"] = presence;
    json["faceCameras"] = faceCameras;
    json["voiceLearning"] = voiceLearning;
    json["cameraAudio"] = cameraAudio;
    json["location"] = location;
    return json;
  }
};

struct PrivacyState
{
  int64_t noticeVersion{0};
  bool decided{false};
  int64_t decidedAt{0};
  int64_t updatedAt{0};
  PrivacyChoices choices;
  PrivacyChoices effective;
};
