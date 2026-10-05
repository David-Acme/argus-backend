#pragma once

#include <auth/user-role.hxx>
#include <config/identity-config.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/repositories/voice-device/voice-device-repository.hxx>
#include <feature/voiceprint/repositories/voice-profile/voice-profile-repository.hxx>
#include <feature/voiceprint/repositories/voice-sample/voice-sample-repository.hxx>
#include <feature/voiceprint/services/audio/voice-audio.hxx>
#include <feature/voiceprint/vocabulary/voiceprint-outcome.hxx>
#include <optional>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/services/privacy/privacy-gate.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <string>
#include <string_view>
#include <vector>

struct VoiceprintIdentifyResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  bool matched{false};
  int64_t userId{0};
  float score{0.0F};
  float threshold{0.0F};
  std::optional<int64_t> personId;
  std::string name;
  std::optional<UserRole> role;
};

struct VoiceprintDirectoryEntry
{
  int64_t userId{0};
  int64_t since{0};
  int64_t updatedAt{0};
};

struct VoiceprintDirectory
{
  bool available{false};
  std::vector<VoiceprintDirectoryEntry> recognized;
};

struct VoiceprintForgetRequest
{
  int64_t actorId{0};
  int64_t subjectId{0};
};

inline constexpr std::string_view kVoiceEraseConsentWithdrawn = "consentWithdrawn";
inline constexpr std::string_view kVoiceEraseBiometrics = "biometricsErased";

struct VoiceprintEraseInput
{
  int64_t actorId{0};
  int64_t subjectId{0};
  std::string_view reason{kVoiceEraseConsentWithdrawn};
  bool byOwner{false};
  drogon::orm::DbClient* client{nullptr};
};

struct VoiceprintEraseResult
{
  std::optional<int64_t> removedProfile;
  size_t samples{0};
};

struct VoiceprintForgetResult
{
  bool hadProfile{false};
  size_t samples{0};
};

class VoiceprintFeatureService
{
public:
  VoiceprintFeatureService();
  explicit VoiceprintFeatureService(IdentityVoiceprintConfig config);

  [[nodiscard]] drogon::Task<VoiceprintIdentifyResult>
  identify(EncodedVoice sample) const;

  [[nodiscard]] drogon::Task<VoiceprintDirectory> directory() const;

  [[nodiscard]] drogon::Task<VoiceprintForgetResult>
  forget(const VoiceprintForgetRequest& request) const;

  [[nodiscard]] drogon::Task<VoiceprintEraseResult>
  eraseForConsent(const VoiceprintEraseInput& input) const;

  static drogon::Task<void> dropFromIndex(VoiceprintEraseResult erased);

  [[nodiscard]] const IdentityVoiceprintConfig& config() const
  {
    return config_;
  }

  [[nodiscard]] static std::string activeModel(const IdentityVoiceprintConfig& config);

private:
  IdentityVoiceprintConfig config_;
  VoiceProfileRepository profileRepository_;
  VoiceSampleRepository sampleRepository_;
  VoiceDeviceRepository deviceRepository_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
  PrivacyGate privacyGate_;
};
