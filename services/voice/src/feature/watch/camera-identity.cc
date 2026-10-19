#include "camera-identity.hxx"

#include <auth/user-role.hxx>
#include <voice/voice-client.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>

namespace
{

constexpr std::string_view kAcceptedFace = "accepted";

std::vector<int16_t> pcmOf(const std::vector<float>& samples)
{
  std::vector<int16_t> out(samples.size());
  std::ranges::transform(samples, out.begin(), [](float sample) {
    return static_cast<int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
  });
  return out;
}

argus::voice::v1::VoiceLanguage languageOf(VoiceLang lang)
{
  return lang == VoiceLang::En ? argus::voice::v1::VOICE_LANGUAGE_EN : argus::voice::v1::VOICE_LANGUAGE_ES;
}

}

std::vector<SeenFaceEvidence> facesOf(const argus::camera::v1::FaceWindowResponse& response)
{
  std::vector<SeenFaceEvidence> faces;
  faces.reserve(static_cast<size_t>(response.faces_size()));
  for (const auto& face : response.faces())
    faces.push_back({.personId = face.person_id(),
                     .userId = face.user_id(),
                     .name = face.name(),
                     .role = face.role(),
                     .confidence = face.confidence(),
                     .accepted = face.quality() == kAcceptedFace,
                     .visitor = face.visitor(),
                     .trusted = face.trusted()});
  return faces;
}

std::optional<HeardVoiceEvidence> CameraIdentityResolver::heard(const std::vector<float>& samples) const
{
  if (!input_.voiceprint || samples.empty())
    return std::nullopt;
  const std::vector<int16_t> pcm = pcmOf(samples);
  const auto answer = input_.voiceprint->identify({.samples = pcm, .sampleRate = 16000});
  if (!answer)
    return std::nullopt;
  return HeardVoiceEvidence{.matched = answer->matched(),
                            .userId = answer->user_id(),
                            .name = answer->has_name() ? answer->name() : std::string(),
                            .score = answer->score()};
}

std::optional<ConversationOpening> CameraIdentityResolver::resolve(const ConfirmedWake& wake) const
{
  SpeakerResolutionInput input{.faces = {}, .voice = std::nullopt, .lang = input_.lang};
  if (input_.camera) {
    if (const auto window = input_.camera->faceWindow(wake.cameraId, static_cast<int>(input_.faceWindow.count())))
      input.faces = facesOf(*window);
  }
  const std::vector<float> samples = wake.capture ? wake.capture->snapshot() : std::vector<float>{};
  input.voice = heard(samples);
  const SpeakerResolution resolution = resolveSpeaker(input);
  if (resolution.disagreement)
    LOG_INFO << "Voice watch: face and voice disagree on camera " << wake.cameraId << "; the guest set applies";
  if (input_.inCall) {
    const bool faceOnCall = resolution.userId > 0 && input_.inCall(resolution.userId);
    const bool voiceOnCall = input.voice && input.voice->matched && input.voice->userId > 0 &&
                             input_.inCall(input.voice->userId);
    if (faceOnCall || voiceOnCall) {
      LOG_INFO << "Voice watch: the wake on camera " << wake.cameraId
               << " comes from a person already on a call with Argus; ignored";
      return std::nullopt;
    }
  }
  ConversationOpening opening;
  opening.audience = resolution.audience;
  opening.surface = resolution.surface;
  opening.notes = resolution.notes;
  opening.faceSeen = !input.faces.empty();
  opening.identity.set_language(languageOf(input_.lang));
  if (resolution.userId > 0) {
    opening.identity.set_user_id(resolution.userId);
    opening.identity.set_name(resolution.name);
    opening.identity.set_role(voiceRoleToProto(parseUserRole(resolution.role).value_or(UserRole::Unknown)));
  }
  LOG_INFO << "Voice watch: camera " << wake.cameraId << " conversation audience "
           << std::string(conversationAudienceToString(resolution.audience));
  return opening;
}

bool CameraIdentityResolver::sameSpeaker(int64_t userId, const std::vector<float>& samples) const
{
  if (userId <= 0)
    return false;
  const auto voice = heard(samples);
  return voice && voice->matched && voice->userId == userId;
}
