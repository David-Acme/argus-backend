#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/voice/voice-session-service.hxx>
#include <feature/watch/camera-conversation.hxx>
#include <feature/watch/camera-gains.hxx>
#include <feature/watch/room-directory.hxx>
#include <feature/watch/stage-two.hxx>
#include <feature/watch/watch-coordinator.hxx>

#include <camera/camera-watch-client.hxx>
#include <guard/guard-duress-client.hxx>
#include <config/voice-config.hxx>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ConversationOpening
{
  argus::voice::v1::VoiceIdentity identity;
  ConversationAudience audience{ConversationAudience::Guest};
  ConversationSurface surface{ConversationSurface::CameraVisitor};
  std::vector<std::string> notes;
};

struct ConversationManagerInput
{
  VoiceWatchConfig config;
  VoiceSessionService* sessions{nullptr};
  std::shared_ptr<const CameraWatchClient> camera;
  std::shared_ptr<EchoGuard> echo;
  std::shared_ptr<RoomDirectory> rooms;
  IVoiceTts* tts{nullptr};
  IVoiceStt* stt{nullptr};
  VoiceLang lang{VoiceLang::Es};
  std::function<std::optional<WatchCameraInfo>(int64_t)> cameraInfo;
  std::function<bool(int64_t)> listening;
  std::function<std::optional<ConversationOpening>(const ConfirmedWake&)> resolve;
  std::function<bool(int64_t, const std::vector<float>&)> sameSpeaker;
  std::function<void(const ConversationSnapshot&)> onUpdate;
  std::function<void(const ConversationEnded&)> onEnded;
  std::function<void(const CameraPanicCall&)> panic;
};

struct HopChoiceInput
{
  int64_t from{0};
  const std::vector<int64_t>& roommates;
  std::function<std::optional<WatchCameraInfo>(int64_t)> cameraInfo;
  std::function<bool(int64_t)> listening;
  std::function<bool(int64_t)> occupied;
};

[[nodiscard]] std::optional<CameraHop> chooseHop(const HopChoiceInput& input);

class ConversationManager
{
public:
  explicit ConversationManager(ConversationManagerInput input);
  ~ConversationManager();

  ConversationManager(const ConversationManager&) = delete;
  ConversationManager& operator=(const ConversationManager&) = delete;

  void start();
  void shutdown();

  void onWake(const ConfirmedWake& wake);
  void onFrame(const EarFrame& frame);
  [[nodiscard]] bool onPlaybackFire(const EarFire& fire);

  [[nodiscard]] std::vector<ConversationSnapshot> list(int64_t userId) const;
  [[nodiscard]] std::optional<ConversationSnapshot> find(const std::string& conversationId) const;
  [[nodiscard]] bool end(int64_t userId, const std::string& conversationId);
  [[nodiscard]] bool occupied(int64_t cameraId) const;
  [[nodiscard]] size_t liveFor(int64_t userId) const;
  [[nodiscard]] std::shared_ptr<CameraConversation> live(const std::string& conversationId) const;
  [[nodiscard]] std::map<int64_t, float> speakerLevels(const std::string& conversationId) const;
  void forget(const std::string& conversationId);

private:
  void reap(const std::stop_token& stop);
  void ended(const ConversationEnded& ended);
  void heard(const std::string& conversationId, const std::string& text);
  [[nodiscard]] std::shared_ptr<CameraConversation> onCamera(int64_t cameraId) const;

  ConversationManagerInput input_;
  StageTwo stageTwo_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<CameraConversation>> live_;
  std::map<std::string, ConversationSnapshot> finished_;
  std::map<std::string, std::shared_ptr<SpeakerLevels>> levels_;
  std::jthread reaper_;
};
