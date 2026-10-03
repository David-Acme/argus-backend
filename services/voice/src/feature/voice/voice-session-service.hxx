#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <feature/voice/voice-engine-seam.hxx>
#include <memory>
#include <mutex>
#include <llm/llm-service.hxx>
#include <shared/services/noise/noise-suppression-service.hxx>
#include <shared/services/reaction/reaction-engine.hxx>
#include <tts/tts-wire.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <audio/audio-resampler.hxx>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <voice/voice-lang.hxx>
#include <voice/reaction-contracts.hxx>
#include <voice/voice-client.hxx>

class VoiceSessionSink
{
public:
  virtual ~VoiceSessionSink() = default;

  virtual bool connected() const = 0;
  virtual void sendServerFrame(argus::voice::v1::ServerFrame frame) = 0;
};

struct PcmFrame
{
  const char* data{nullptr};
  size_t size{0};
};

class VoiceSessionService
{
public:
  explicit VoiceSessionService(const VoiceEngineSeam& engines = {});

  void start(VoiceSessionSink& sink,
             const argus::voice::v1::VoiceStart& request);
  void start(VoiceSessionSink& sink,
             const argus::voice::v1::VoiceIdentity& identity);
  void feedPcm(VoiceSessionSink& sink, const PcmFrame& frame);
  void stop(VoiceSessionSink& sink);
  void skip(VoiceSessionSink& sink);

private:
  struct DuplexTurn
  {
    int64_t id{0};
    bool running{false};
    bool announced{false};
    bool barged{false};
    std::chrono::steady_clock::time_point firstAudioAt{};
    std::chrono::steady_clock::time_point playbackEnd{};
  };

  struct ListenState
  {
    bool listening{false};
    bool armed{false};
  };

  struct AssistantSend
  {
    argus::voice::v1::ServerFrame frame;
    size_t samples{0};
  };

  struct Session
  {
    explicit Session(std::unique_ptr<VadModel> model) : vad(std::move(model)) {}

    VoiceSessionSink* sink{nullptr};
    VadService vad;
    NoiseSuppressor denoiser;
    bool denoise{true};
    float denoiseGateRms{0.0035F};
    int denoiseLogCounter{0};
    std::vector<ChatMessage> history;
    VoiceLang lang{VoiceLang::System};
    int64_t userId{0};
    std::string role;
    bool nameKnown{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> interrupt{false};
    std::atomic<bool> active{true};
    std::mutex turnMutex;
    std::stop_source turnStop;
    std::thread worker;
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    std::vector<float> pcmQueue;
    bool duplex{false};
    std::chrono::milliseconds bargeGuard{300};
    std::thread turnThread;
    std::mutex duplexMutex;
    DuplexTurn turn;
  };

  void workerLoop(std::shared_ptr<Session> session);
  void duplexLoop(const std::shared_ptr<Session>& session);
  std::vector<float> cleanBatch(Session& session, std::vector<float>& batch);
  void launchTurn(const std::shared_ptr<Session>& session,
                  std::function<void(Session&)> body);
  ListenState listenState(Session& session);
  void bargeIn(Session& session);
  void sendDuplexChunk(Session& session, argus::voice::v1::ServerFrame frame);
  void sendDuplexAssistant(Session& session, AssistantSend send);
  void processTurn(Session& session, const std::vector<float>& samples);
  Reaction emitReaction(Session& session, const ReactionSignals& signals);
  void speak(Session& session, const std::string& text);
  void sendFrame(Session& session, argus::voice::v1::ServerFrame frame) const;

  mutable std::mutex mutex_;
  std::unordered_map<VoiceSessionSink*, std::shared_ptr<Session>> sessions_;
  ReactionEngine reactions_;
  IVoiceStt& stt_;
  IVoiceTts& tts_;
  IVoiceLlm& llm_;
  IVoiceIdentity& identity_;
  IVoiceVad& vad_;

  friend struct VoiceSessionTestAccess;
};

VoiceLang voiceSystemLang();

struct VoiceListeningConfig
{
  bool denoise{true};
  float denoiseGateRms{0.0035F};
  std::chrono::milliseconds bargeGuard{300};
};

[[nodiscard]] VoiceListeningConfig resolveVoiceListeningConfig();
