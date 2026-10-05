#pragma once

#include "episode-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class EpisodeRepository
{
public:
  [[nodiscard]] drogon::Task<std::vector<EpisodeRow>> list(const EpisodeListInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<EpisodeRow>> find(int64_t encounterId) const;

  [[nodiscard]] drogon::Task<std::vector<EpisodeTimelineRow>>
  timeline(int64_t encounterId) const;

  [[nodiscard]] drogon::Task<std::vector<TamperRow>>
  tamper(const EpisodeListInput& input) const;

  [[nodiscard]] drogon::Task<bool> recordInsight(const EpisodeInsightInput& input) const;

  [[nodiscard]] drogon::Task<bool> linkGroup(int64_t encounterId, int64_t groupId) const;

  [[nodiscard]] drogon::Task<bool> review(const EpisodeReviewInput& input) const;
  [[nodiscard]] drogon::Task<bool> retain(const EpisodeRetainInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<CameraNotification>>
  recentCameraNotification(const CameraNotificationLookupInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<DigestRow>>
  digest(const DigestWindowInput& input) const;
};
