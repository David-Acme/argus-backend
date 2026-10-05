#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <optional>
#include <shared/repositories/privacy/privacy-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/privacy/privacy-gate.hxx>
#include <shared/vocabulary/privacy-choices.hxx>
#include <vector>

struct PrivacyView
{
  PrivacyState state;
  PrivacyChoices household;
};

struct PrivacyDecisionInput
{
  int64_t userId{0};
  int64_t noticeVersion{0};
  PrivacyChoices choices;
};

struct PrivacyDirectoryEntry
{
  int64_t userId{0};
  PrivacyState state;
};

struct PrivacyDirectory
{
  PrivacyChoices household;
  bool visitorRecognition{false};
  std::optional<int64_t> visitorAcknowledgedAt;
  std::optional<int64_t> householdUpdatedAt;
  std::vector<PrivacyDirectoryEntry> users;
};

struct HouseholdPrivacyChange
{
  int64_t actorId{0};
  std::optional<bool> presence;
  std::optional<bool> faceCameras;
  std::optional<bool> voiceLearning;
  std::optional<bool> cameraAudio;
  std::optional<bool> visitorRecognition;
};

class PrivacyFeatureService
{
public:
  PrivacyFeatureService() = default;

  [[nodiscard]] drogon::Task<PrivacyView> me(int64_t userId) const;

  [[nodiscard]] drogon::Task<PrivacyView>
  decide(const PrivacyDecisionInput& input) const;

  [[nodiscard]] drogon::Task<PrivacyDirectory> directory() const;

  [[nodiscard]] drogon::Task<PrivacyDirectory>
  updateHousehold(const HouseholdPrivacyChange& change) const;

private:
  struct PublishInput
  {
    const UserSchema& user;
    const PrivacyState& state;
    drogon::orm::DbClient* client{nullptr};
  };

  [[nodiscard]] static drogon::Task<void> publishCatalog(PublishInput input);

  PrivacyRepository repository_;
  UserRepository userRepository_;
  PrivacyGate gate_;
  VoiceprintFeatureService voiceprint_;
};
