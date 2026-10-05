#include "voiceprint-client.hxx"

#include <bit>
#include <grpc/grpc-client-base.hxx>

namespace
{

constexpr int kCallTimeoutMs = 5000;
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 48000;
constexpr size_t kMaxCallKeyLength = 128;
constexpr size_t kMaxDeviceHashLength = 256;

bool clipUsable(const VoiceClipView& clip)
{
  return !clip.samples.empty() && clip.sampleRate >= kMinSampleRate &&
         clip.sampleRate <= kMaxSampleRate;
}

bool callKeyUsable(const std::string& callKey)
{
  return !callKey.empty() && callKey.size() <= kMaxCallKeyLength;
}

void fillClip(const VoiceClipView& clip, argus::identity::v1::VoiceClip* out)
{
  std::string bytes;
  bytes.resize(clip.samples.size() * 2);
  auto cursor = bytes.begin();
  for (const int16_t sample : clip.samples) {
    const auto word = std::bit_cast<uint16_t>(sample);
    *cursor++ = static_cast<char>(word & 0xFFU);
    *cursor++ = static_cast<char>((word >> 8U) & 0xFFU);
  }
  out->set_pcm16(std::move(bytes));
  out->set_sample_rate(clip.sampleRate);
}

struct CallSetup
{
  grpc::ClientContext& context;
  const argus::client::PeerCredential& credential;
  int timeoutMs{kCallTimeoutMs};
};

void prepare(const CallSetup& setup)
{
  argus::client::setDeadline(setup.context, setup.timeoutMs);
  argus::client::addPeerCredential(setup.context, setup.credential);
}

}

VoiceprintClient::VoiceprintClient(VoiceprintClientConfig config)
    : credential_({.credential = std::move(config.credential),
                   .fleetSecret = std::move(config.fleetSecret)}),
      channel_(argus::client::makeChannel(config.target)),
      stub_(argus::identity::v1::VoiceprintService::NewStub(channel_))
{
}

std::optional<argus::identity::v1::IdentifyVoiceResponse>
VoiceprintClient::identify(const VoiceClipView& sample) const
{
  return identifyWithin({.sample = sample, .timeoutMs = kCallTimeoutMs});
}

std::optional<argus::identity::v1::IdentifyVoiceResponse>
VoiceprintClient::identifyWithin(const VoiceprintIdentifyInput& input) const
{
  if (!clipUsable(input.sample) || input.timeoutMs <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .credential = credential_,
           .timeoutMs = input.timeoutMs});

  argus::identity::v1::IdentifyVoiceRequest request;
  fillClip(input.sample, request.mutable_sample());

  argus::identity::v1::IdentifyVoiceResponse response;
  if (const grpc::Status status = stub_->Identify(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::IdentifyVoiceResponse>
VoiceprintClient::observeTurn(const VoiceTurnObservation& input) const
{
  if (!clipUsable(input.sample) || input.timeoutMs <= 0 || input.userId <= 0 ||
      input.deviceHash.empty() ||
      input.deviceHash.size() > kMaxDeviceHashLength ||
      !callKeyUsable(input.callKey))
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .credential = credential_,
           .timeoutMs = input.timeoutMs});

  argus::identity::v1::ObserveVoiceTurnRequest request;
  fillClip(input.sample, request.mutable_sample());
  request.set_user_id(input.userId);
  request.set_device_hash(input.deviceHash);
  request.set_call_key(input.callKey);

  argus::identity::v1::IdentifyVoiceResponse response;
  if (const grpc::Status status =
          stub_->ObserveTurn(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

bool VoiceprintClient::closeCall(const VoiceCallClose& input) const
{
  if (!callKeyUsable(input.callKey) || input.timeoutMs <= 0)
    return false;

  grpc::ClientContext context;
  prepare({.context = context,
           .credential = credential_,
           .timeoutMs = input.timeoutMs});

  argus::identity::v1::CloseVoiceCallRequest request;
  request.set_call_key(input.callKey);

  argus::identity::v1::CloseVoiceCallResponse response;
  const grpc::Status status = stub_->CloseCall(&context, request, &response);
  return status.ok() && response.closed();
}
