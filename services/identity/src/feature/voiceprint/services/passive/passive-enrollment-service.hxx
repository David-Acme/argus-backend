#pragma once

#include <config/identity-config.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/repositories/voice-device/voice-device-repository.hxx>
#include <feature/voiceprint/repositories/voice-profile/voice-profile-repository.hxx>
#include <feature/voiceprint/repositories/voice-sample/voice-sample-repository.hxx>
#include <feature/voiceprint/services/audio/voice-audio.hxx>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/services/passive/passive-policy.hxx>
#include <feature/voiceprint/services/passive/voice-call-tracker.hxx>
#include <memory>
#include <optional>
#include <shared/repositories/user/user-repository.hxx>
#include <string>
#include <vector>

struct VoiceTurnInput
{
  int64_t userId{0};
  std::string deviceHash;
  std::string callKey;
  EncodedVoice sample;
  int64_t now{0};
};

struct VoiceTurnLearning
{
  bool considered{false};
  VoiceAnalysisStatus quality{VoiceAnalysisStatus::Invalid};
  float speechSeconds{0.0F};
  std::optional<TurnVerdict> verdict;
  std::optional<float> halvesScore;
  std::optional<float> ownScore;
  std::optional<OtherVoiceMatch> bestOther;
};

struct VoiceCallCloseInput
{
  std::string callKey;
  int64_t now{0};
};

enum class PassiveCallOutcome : uint8_t
{
  Unavailable = 0,
  NotFound,
  Tainted,
  Unusable,
  Inactive,
  OtherVoice,
  Pending,
  Adopted,
  Refreshed,
  Linked,
  Relinked
};

[[nodiscard]] const char* passiveCallOutcomeName(PassiveCallOutcome outcome);

class PassiveEnrollmentService
{
public:
  PassiveEnrollmentService();
  explicit PassiveEnrollmentService(IdentityVoiceprintConfig config);

  [[nodiscard]] drogon::Task<VoiceTurnLearning>
  learnFromTurn(VoiceTurnInput input) const;

  [[nodiscard]] drogon::Task<PassiveCallOutcome>
  closeCall(const VoiceCallCloseInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<PassiveCallOutcome>>
  expireCalls(int64_t now) const;

  [[nodiscard]] drogon::Task<PassiveCallOutcome>
  recordCall(const ClosedCall& call, int64_t now) const;

  [[nodiscard]] drogon::Task<void> purgeExpired(int64_t now) const;

  [[nodiscard]] size_t openCalls() const { return tracker_->open(); }

private:
  struct CallContext
  {
    const ClosedCall& call;
    const std::vector<float>& centroid;
    const std::string& model;
    int64_t now{0};
  };

  struct Scores
  {
    std::optional<float> own;
    std::optional<OtherVoiceMatch> bestOther;
  };

  [[nodiscard]] static Scores scoresFor(std::span<const float> embedding,
                                        int64_t userId);

  [[nodiscard]] drogon::Task<PassiveCallOutcome>
  refreshProfile(const CallContext& context,
                 const VoiceProfileSchema& profile) const;

  [[nodiscard]] drogon::Task<PassiveCallOutcome>
  evaluateLink(const CallContext& context,
               const std::optional<VoiceProfileSchema>& profile) const;

  IdentityVoiceprintConfig config_;
  PassivePolicy policy_;
  std::shared_ptr<VoiceCallTracker> tracker_;
  VoiceProfileRepository profileRepository_;
  VoiceSampleRepository sampleRepository_;
  VoiceDeviceRepository deviceRepository_;
  UserRepository userRepository_;
};
