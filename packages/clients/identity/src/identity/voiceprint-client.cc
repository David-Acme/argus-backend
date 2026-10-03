#include "voiceprint-client.hxx"

#include <grpc/grpc-client-base.hxx>

#include <algorithm>
#include <bit>

namespace
{

constexpr int kCallTimeoutMs = 5000;
constexpr int kEnrollTimeoutMs = 20000;
constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 48000;

bool clipUsable(const VoiceClipView& clip)
{
  return !clip.samples.empty() && clip.sampleRate >= kMinSampleRate &&
         clip.sampleRate <= kMaxSampleRate;
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

void addSession(grpc::ClientContext& context, const VoiceprintSession& session)
{
  context.AddMetadata("authorization", "Bearer " + session.accessToken);
  if (!session.deviceHash.empty())
    context.AddMetadata("x-argus-device", session.deviceHash);
}

struct CallSetup
{
  grpc::ClientContext& context;
  const std::string& fleetSecret;
  int timeoutMs{kCallTimeoutMs};
};

void prepare(const CallSetup& setup)
{
  argus::client::setDeadline(setup.context, setup.timeoutMs);
  argus::client::addFleetSecret(setup.context, setup.fleetSecret);
}

}

VoiceprintClient::VoiceprintClient(VoiceprintClientConfig config)
    : fleetSecret_(std::move(config.fleetSecret)),
      channel_(argus::client::makeChannel(config.target)),
      stub_(argus::identity::v1::VoiceprintService::NewStub(channel_))
{
}

std::optional<argus::identity::v1::CreateVoiceprintChallengeResponse>
VoiceprintClient::createChallenge(const VoiceprintChallengeInput& input) const
{
  if (input.userId <= 0 || input.session.accessToken.empty())
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kCallTimeoutMs});
  addSession(context, input.session);

  argus::identity::v1::CreateVoiceprintChallengeRequest request;
  request.set_user_id(input.userId);
  if (!input.lang.empty())
    request.set_lang(input.lang);

  argus::identity::v1::CreateVoiceprintChallengeResponse response;
  if (const grpc::Status status =
          stub_->CreateChallenge(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::EnrollVoiceprintResponse>
VoiceprintClient::enroll(const VoiceprintEnrollInput& input) const
{
  if (input.userId <= 0 || input.session.accessToken.empty() ||
      input.samples.empty() || input.challengeId.empty() ||
      !std::ranges::all_of(input.samples, clipUsable))
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kEnrollTimeoutMs});
  addSession(context, input.session);

  argus::identity::v1::EnrollVoiceprintRequest request;
  request.set_user_id(input.userId);
  for (const auto& sample : input.samples)
    fillClip(sample, request.add_samples());
  request.set_consent(input.consent);
  request.set_consent_version(input.consentVersion);
  request.set_challenge_id(input.challengeId);
  if (!input.faceImage.empty())
    request.set_face_image(input.faceImage);

  argus::identity::v1::EnrollVoiceprintResponse response;
  if (const grpc::Status status = stub_->Enroll(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::VerifyVoiceprintResponse>
VoiceprintClient::verify(const VoiceprintVerifyInput& input) const
{
  if (input.userId <= 0 || !clipUsable(input.sample))
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kCallTimeoutMs});

  argus::identity::v1::VerifyVoiceprintRequest request;
  request.set_user_id(input.userId);
  fillClip(input.sample, request.mutable_sample());

  argus::identity::v1::VerifyVoiceprintResponse response;
  if (const grpc::Status status = stub_->Verify(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::IdentifyVoiceResponse>
VoiceprintClient::identify(const VoiceClipView& sample) const
{
  if (!clipUsable(sample))
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kCallTimeoutMs});

  argus::identity::v1::IdentifyVoiceRequest request;
  fillClip(sample, request.mutable_sample());

  argus::identity::v1::IdentifyVoiceResponse response;
  if (const grpc::Status status =
          stub_->Identify(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::DeleteVoiceprintResponse>
VoiceprintClient::remove(const VoiceprintDeleteInput& input) const
{
  if (input.userId <= 0 || input.session.accessToken.empty())
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kCallTimeoutMs});
  addSession(context, input.session);

  argus::identity::v1::DeleteVoiceprintRequest request;
  request.set_user_id(input.userId);

  argus::identity::v1::DeleteVoiceprintResponse response;
  if (const grpc::Status status = stub_->Delete(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}

std::optional<argus::identity::v1::GetVoiceprintStatusResponse>
VoiceprintClient::status(int64_t userId) const
{
  if (userId <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  prepare({.context = context,
           .fleetSecret = fleetSecret_,
           .timeoutMs = kCallTimeoutMs});

  argus::identity::v1::GetVoiceprintStatusRequest request;
  request.set_user_id(userId);

  argus::identity::v1::GetVoiceprintStatusResponse response;
  if (const grpc::Status status =
          stub_->GetStatus(&context, request, &response);
      !status.ok())
    return std::nullopt;
  return response;
}
