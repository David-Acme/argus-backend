#pragma once

#include <argus/identity/v1/voiceprint.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct VoiceprintClientConfig
{
  std::string target;
  std::string fleetSecret;
};

struct VoiceClipView
{
  std::span<const int16_t> samples;
  int sampleRate{16000};
};

struct VoiceprintSession
{
  std::string accessToken;
  std::string deviceHash;
};

struct VoiceprintChallengeInput
{
  int64_t userId{0};
  std::string lang;
  VoiceprintSession session;
};

struct VoiceprintEnrollInput
{
  int64_t userId{0};
  std::vector<VoiceClipView> samples;
  bool consent{false};
  std::string consentVersion;
  std::string challengeId;
  std::string faceImage;
  VoiceprintSession session;
};

struct VoiceprintVerifyInput
{
  int64_t userId{0};
  VoiceClipView sample;
};

struct VoiceprintDeleteInput
{
  int64_t userId{0};
  VoiceprintSession session;
};

class VoiceprintClient
{
public:
  explicit VoiceprintClient(VoiceprintClientConfig config);

  VoiceprintClient(const VoiceprintClient&) = delete;
  VoiceprintClient& operator=(const VoiceprintClient&) = delete;
  virtual ~VoiceprintClient() = default;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::CreateVoiceprintChallengeResponse>
  createChallenge(const VoiceprintChallengeInput& input) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::EnrollVoiceprintResponse>
  enroll(const VoiceprintEnrollInput& input) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::VerifyVoiceprintResponse>
  verify(const VoiceprintVerifyInput& input) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::IdentifyVoiceResponse>
  identify(const VoiceClipView& sample) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::DeleteVoiceprintResponse>
  remove(const VoiceprintDeleteInput& input) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::GetVoiceprintStatusResponse>
  status(int64_t userId) const;

private:
  std::string fleetSecret_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::VoiceprintService::StubInterface> stub_;
};
