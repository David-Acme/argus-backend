#pragma once

#include <feature/watch/speaker-resolution.hxx>
#include <feature/watch/watch-conversations.hxx>

#include <camera/camera-watch-client.hxx>
#include <identity/voiceprint-client.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct CameraIdentityInput
{
  std::shared_ptr<const CameraWatchClient> camera;
  std::shared_ptr<const VoiceprintClient> voiceprint;
  std::chrono::milliseconds faceWindow{1500};
  VoiceLang lang{VoiceLang::Es};
  std::function<bool(int64_t)> inCall;
};

[[nodiscard]] std::vector<SeenFaceEvidence> facesOf(const argus::camera::v1::FaceWindowResponse& response);

class CameraIdentityResolver
{
public:
  explicit CameraIdentityResolver(CameraIdentityInput input) : input_(std::move(input)) {}

  [[nodiscard]] std::optional<ConversationOpening> resolve(const ConfirmedWake& wake) const;
  [[nodiscard]] bool sameSpeaker(int64_t userId, const std::vector<float>& samples) const;

private:
  [[nodiscard]] std::optional<HeardVoiceEvidence> heard(const std::vector<float>& samples) const;

  CameraIdentityInput input_;
};
