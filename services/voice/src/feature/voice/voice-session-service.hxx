#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <atomic>
#include <condition_variable>
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
             const argus::voice::v1::VoiceIdentity& identity);
  void feedPcm(VoiceSessionSink& sink, const PcmFrame& frame);
  void stop(VoiceSessionSink& sink);
  void skip(VoiceSessionSink& sink);

private:
  struct Session
  {
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
    std::mutex ttsMutex;
    std::stop_source ttsStop;
    std::thread worker;
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    std::vector<float> pcmQueue;
  };

  void workerLoop(std::shared_ptr<Session> session);
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

  friend struct VoiceSessionTestAccess;
};

VoiceLang voiceSystemLang();
