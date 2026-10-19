#pragma once

#include <feature/voice/voice-engine-seam.hxx>
#include <feature/voice/voice-session-service.hxx>
#include <feature/watch/camera-ear.hxx>
#include <feature/watch/conversation-policy.hxx>
#include <feature/watch/echo-guard.hxx>

#include <argus/voice/v1/voice.pb.h>
#include <camera/camera-watch-client.hxx>
#include <config/voice-config.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct CameraHop
{
  int64_t cameraId{0};
  std::string name;
};

struct ConversationEnded
{
  ConversationSnapshot snapshot;
  ConversationEnd reason{ConversationEnd::None};
};

struct CameraConversationInput
{
  std::string id;
  int64_t cameraId{0};
  std::string cameraName;
  argus::voice::v1::VoiceStart start;
  ConversationAudience audience{ConversationAudience::Guest};
  ConversationSurface surface{ConversationSurface::CameraVisitor};
  VoiceSessionService* sessions{nullptr};
  std::shared_ptr<const CameraWatchClient> camera;
  std::shared_ptr<EchoGuard> echo;
  IVoiceTts* tts{nullptr};
  VoiceWatchConfig config;
  VoiceLang lang{VoiceLang::Es};
  std::vector<std::string> notes;
  std::function<void(const ConversationSnapshot&)> onUpdate;
  std::function<std::optional<CameraHop>(int64_t)> nextCamera;
  std::function<void(const ConversationEnded&)> onEnded;
  std::function<void(const std::string&)> onUserText;
};

[[nodiscard]] std::vector<int16_t> pcm16Of(const std::string& bytes);
[[nodiscard]] std::vector<float> floatsOf(std::span<const int16_t> samples);
[[nodiscard]] int64_t steadyMs();

class CameraConversation final : public VoiceSessionSink
{
public:
  explicit CameraConversation(CameraConversationInput input);
  ~CameraConversation() override;

  CameraConversation(const CameraConversation&) = delete;
  CameraConversation& operator=(const CameraConversation&) = delete;

  void open(const std::vector<float>& initialAudio);
  void feed(const EarFrame& frame);
  void bargeIn(const std::vector<float>& heard);
  void requestEnd(ConversationEnd reason);
  void join();

  [[nodiscard]] bool closed() const { return closed_.load(); }
  [[nodiscard]] int64_t cameraId() const { return cameraId_.load(); }
  [[nodiscard]] const std::string& id() const { return input_.id; }
  [[nodiscard]] int64_t userId() const { return input_.start.identity().user_id(); }
  [[nodiscard]] ConversationSnapshot snapshot() const;
  [[nodiscard]] bool speaking() const;

  [[nodiscard]] bool connected() const override;
  void sendServerFrame(argus::voice::v1::ServerFrame frame) override;
  [[nodiscard]] int playoutRate() const override { return kWatchRate; }
  [[nodiscard]] std::string shapeSpeech(const std::string& text) const override;
  [[nodiscard]] bool admitsTurn(const std::string& text, VoiceSpeakerVerdict verdict) override;

private:
  void tick(const std::stop_token& stop);
  void openSpeaker(bool earcon);
  void writeSpeaker(std::span<const int16_t> pcm);
  void onSpeakerEvent(const argus::camera::v1::SpeakEvent& event);
  void finishSpeaker(bool cancel);
  [[nodiscard]] bool hop();
  void sayDirect(const std::string& line);
  void close(ConversationEnd reason);
  void addTurn(const std::string& role, const std::string& text);
  void setState(ConversationState state);
  void publish();

  CameraConversationInput input_;
  std::atomic<int64_t> cameraId_;
  mutable std::mutex mutex_;
  ConversationSnapshot snapshot_;
  std::shared_ptr<WatchSpeakerStream> speaker_;
  std::vector<int16_t> replyPcm_;
  int64_t lastChunkAt_{0};
  int64_t lastFrameAt_{0};
  int64_t lastSpeechAt_{0};
  int64_t openedAt_{0};
  int64_t userTurnAt_{0};
  int64_t firstAudioAt_{0};
  int64_t nextTurnId_{1};
  bool speakerEnding_{false};
  bool speakerDone_{false};
  bool speakerBusy_{false};
  bool speakerLost_{false};
  bool acked_{false};
  bool closeAfterReply_{false};
  bool delegated_{false};
  ConversationEnd requested_{ConversationEnd::None};
  std::atomic<bool> started_{false};
  std::atomic<bool> closed_{false};
  std::jthread ticker_;
};
