#pragma once

#include <argus/identity/v1/voiceprint.grpc.pb.h>
#include <cstdint>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <span>
#include <string>

struct VoiceprintClientConfig
{
  std::string target;
  std::string credential{};
  std::string fleetSecret;
};

struct VoiceClipView
{
  std::span<const int16_t> samples;
  int sampleRate{16000};
};

struct VoiceprintIdentifyInput
{
  VoiceClipView sample;
  int timeoutMs{5000};
};

struct VoiceTurnObservation
{
  VoiceClipView sample;
  int64_t userId{0};
  std::string deviceHash;
  std::string callKey;
  int timeoutMs{5000};
};

struct VoiceCallClose
{
  std::string callKey;
  int timeoutMs{5000};
};

class VoiceprintClient
{
public:
  explicit VoiceprintClient(VoiceprintClientConfig config);

  VoiceprintClient(const VoiceprintClient&) = delete;
  VoiceprintClient& operator=(const VoiceprintClient&) = delete;
  virtual ~VoiceprintClient() = default;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::IdentifyVoiceResponse>
  identify(const VoiceClipView& sample) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::IdentifyVoiceResponse>
  identifyWithin(const VoiceprintIdentifyInput& input) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::IdentifyVoiceResponse>
  observeTurn(const VoiceTurnObservation& input) const;

  [[nodiscard]] virtual bool closeCall(const VoiceCallClose& input) const;

private:
  argus::client::PeerCredential credential_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::VoiceprintService::StubInterface> stub_;
};
