#pragma once

#include <auth/user-role.hxx>
#include <config/identity-config.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/repositories/voiceprint-challenge/voiceprint-challenge-repository.hxx>
#include <feature/voiceprint/repositories/voiceprint/voiceprint-repository.hxx>
#include <feature/voiceprint/services/audio/voice-audio.hxx>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/vocabulary/voiceprint-method.hxx>
#include <feature/voiceprint/vocabulary/voiceprint-outcome.hxx>
#include <optional>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <string>
#include <string_view>
#include <vector>
#include <voice/voice-lang.hxx>

inline constexpr std::string_view kVoiceprintConsentVersion =
    "voiceprint-consent-v1";

struct VoiceprintActor
{
  int64_t userId{0};
  UserRole role{UserRole::Guest};
  std::string deviceHash;
};

struct VoiceprintStatusView
{
  bool available{false};
  bool enrolled{false};
  bool stale{false};
  std::string model;
  int sampleCount{0};
  int64_t enrolledAt{0};
  VoiceprintMethod method{VoiceprintMethod::Self};
  std::string consentVersion;
  int samplesRequired{0};
  float minSpeechSeconds{0.0F};
};

struct VoiceprintStatusResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  VoiceprintStatusView status;
};

struct VoiceprintChallengeRequest
{
  VoiceprintActor actor;
  int64_t subjectId{0};
  std::optional<VoiceLang> lang;
};

struct VoiceprintChallengeView
{
  std::string challengeId;
  std::vector<std::string> phrases;
  int64_t expiresAt{0};
  VoiceLang lang{VoiceLang::Es};
};

struct VoiceprintChallengeResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  VoiceprintChallengeView challenge;
};

struct VoiceprintEnrollRequest
{
  VoiceprintActor actor;
  int64_t subjectId{0};
  std::vector<EncodedVoice> samples;
  bool consent{false};
  std::string consentVersion;
  std::string challengeId;
  std::string faceImage;
};

struct VoiceprintEnrollResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  VoiceprintStatusView status;
  std::optional<int> failedSample;
};

struct VoiceprintVerifyRequest
{
  int64_t userId{0};
  EncodedVoice sample;
};

struct VoiceprintVerifyResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  bool matched{false};
  float score{0.0F};
  float threshold{0.0F};
};

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

struct VoiceprintSampleCheck
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  float speechSeconds{0.0F};
  float snrDb{0.0F};
  float minSpeechSeconds{0.0F};
  float minSnrDb{0.0F};
};

struct VoiceprintDeleteRequest
{
  VoiceprintActor actor;
  int64_t subjectId{0};
};

struct VoiceprintDeleteResult
{
  VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
  bool deleted{false};
};

class VoiceprintFeatureService
{
public:
  VoiceprintFeatureService();
  explicit VoiceprintFeatureService(IdentityVoiceprintConfig config);

  [[nodiscard]] drogon::Task<VoiceprintStatusResult>
  status(int64_t userId) const;

  [[nodiscard]] drogon::Task<VoiceprintChallengeResult>
  createChallenge(const VoiceprintChallengeRequest& request) const;

  [[nodiscard]] drogon::Task<VoiceprintSampleCheck>
  checkSample(EncodedVoice sample) const;

  [[nodiscard]] drogon::Task<VoiceprintEnrollResult>
  enroll(VoiceprintEnrollRequest request) const;

  [[nodiscard]] drogon::Task<VoiceprintVerifyResult>
  verify(VoiceprintVerifyRequest request) const;

  [[nodiscard]] drogon::Task<VoiceprintIdentifyResult>
  identify(EncodedVoice sample) const;

  [[nodiscard]] drogon::Task<VoiceprintDeleteResult>
  remove(const VoiceprintDeleteRequest& request) const;

  [[nodiscard]] static std::string hashToken(const std::string& token);

  [[nodiscard]] const IdentityVoiceprintConfig& config() const
  {
    return config_;
  }

private:
  struct SubjectAccess
  {
    VoiceprintActor actor;
    int64_t subjectId{0};
    bool requireActive{true};
  };

  struct SubjectCheck
  {
    VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
    std::optional<UserSchema> subject;
  };

  struct AnalyzedSamples
  {
    VoiceprintOutcome outcome{VoiceprintOutcome::Ok};
    std::optional<int> failedSample;
    std::vector<std::vector<float>> embeddings;
    double speechSeconds{0.0};
  };

  [[nodiscard]] drogon::Task<SubjectCheck>
  manageableSubject(const SubjectAccess& access) const;

  [[nodiscard]] drogon::Task<AnalyzedSamples>
  analyzeSamples(std::vector<EncodedVoice> samples) const;

  [[nodiscard]] drogon::Task<bool>
  faceBelongsTo(const VoiceprintEnrollRequest& request) const;

  [[nodiscard]] drogon::Task<bool>
  challengeUsable(const VoiceprintEnrollRequest& request) const;

  [[nodiscard]] VoiceprintStatusView
  viewOf(const std::optional<VoiceprintSchema>& voiceprint) const;

  [[nodiscard]] std::optional<int>
  inconsistentSample(const std::vector<std::vector<float>>& embeddings) const;

  [[nodiscard]] bool voiceTakenByOther(const std::vector<float>& centroid,
                                       int64_t subjectId) const;

  IdentityVoiceprintConfig config_;
  VoiceprintRepository voiceprintRepository_;
  VoiceprintChallengeRepository challengeRepository_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
};
