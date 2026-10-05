#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/guard/dtos/create-environment-dto.hxx>
#include <feature/guard/dtos/create-expected-guest-dto.hxx>
#include <feature/guard/dtos/list-decisions-dto.hxx>
#include <feature/guard/dtos/list-episodes-dto.hxx>
#include <feature/guard/dtos/update-camera-context-dto.hxx>
#include <feature/guard/dtos/update-environment-dto.hxx>
#include <feature/guard/dtos/update-guard-mode-dto.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schedule.hxx>
#include <feature/guard/services/disarm-gate.hxx>
#include <feature/guard/repositories/camera-context/camera-context-repository.hxx>
#include <feature/guard/repositories/episode/episode-repository.hxx>
#include <feature/guard/repositories/environment/environment-repository.hxx>
#include <feature/guard/vocabulary/feedback-label.hxx>
#include <optional>
#include <json/value.h>
#include <string>

class IdentityClient;

struct GuardFeatureDependencies
{
  IdentityClient* identity{nullptr};
  const DisarmGate* disarm{nullptr};
};

struct GuardModeChange
{
  const UpdateGuardModeDto& body;
  int64_t userId{0};
  std::string userName;
};

class GuardFeatureService
{
public:
  struct PromotePersonInput
  {
    int64_t personId{0};
    std::string accessToken;
    std::string deviceHash;
  };

  explicit GuardFeatureService(const GuardFeatureDependencies& dependencies);

  drogon::Task<bool> promotePerson(const PromotePersonInput& input) const;

  [[nodiscard]] drogon::Task<Json::Value> environments() const;

  [[nodiscard]] drogon::Task<Json::Value>
  createEnvironment(const CreateEnvironmentDto& input) const;

  struct EnvironmentPatchInput
  {
    int64_t id{0};
    const UpdateEnvironmentDto& patch;
  };

  [[nodiscard]] drogon::Task<Json::Value>
  updateEnvironment(const EnvironmentPatchInput& input) const;

  [[nodiscard]] drogon::Task<Json::Value> removeEnvironment(int64_t id) const;

  [[nodiscard]] drogon::Task<Json::Value> setMode(const GuardModeChange& input) const;

  drogon::Task<Json::Value> incidents(int limit) const;

  drogon::Task<Json::Value> decisions(const ListDecisionsDto& query) const;

  drogon::Task<Json::Value>
  decisionsSummary(const DecisionsSummaryInput& input) const;

  drogon::Task<bool> setFeedback(const std::string& eventId,
                                 const std::string& label) const;

  drogon::Task<int64_t> createGuest(
      const CreateExpectedGuestDto& input) const;

  drogon::Task<Json::Value> guests() const;

  drogon::Task<bool> removeGuest(int64_t id) const;

  [[nodiscard]] drogon::Task<Json::Value> cameras() const;

  struct CameraContextInput
  {
    int64_t cameraId{0};
    const UpdateCameraContextDto& context;
  };

  [[nodiscard]] drogon::Task<Json::Value>
  setCamera(const CameraContextInput& input) const;

  [[nodiscard]] drogon::Task<Json::Value>
  episodes(const ListEpisodesDto& query) const;

  [[nodiscard]] drogon::Task<std::optional<Json::Value>>
  episode(int64_t id) const;

  struct ReviewInput
  {
    int64_t episodeId{0};
    FeedbackLabel label{FeedbackLabel::Useful};
  };

  [[nodiscard]] drogon::Task<std::optional<Json::Value>>
  reviewEpisode(const ReviewInput& input) const;

private:
  [[nodiscard]] drogon::Task<void>
  requireNameFree(const std::string& name, int64_t except) const;

  [[nodiscard]] drogon::Task<int64_t> resolveEnvironment(int64_t id) const;

  IdentityClient* identity_{nullptr};
  const DisarmGate* disarm_{nullptr};
  GuardRepository guardRepository_;
  EnvironmentRepository environmentRepository_;
  CameraContextRepository cameraContextRepository_;
  EpisodeRepository episodeRepository_;
};
