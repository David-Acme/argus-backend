#pragma once

#include <atomic>
#include <condition_variable>
#include <drogon/WebSocketController.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <shared/enums.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/noise/noise-suppression-service.hxx>
#include <shared/services/reaction/reaction-engine.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <feature/socket/sync/services/voice-engine-seam.hxx>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// One voice session per WebSocket connection. Binary frames on `/sync` are
// ALWAYS audio: incoming = 16 kHz s16le PCM from the app mic, outgoing =
// 16 kHz s16le PCM of the assistant reply. The session language is resolved
// per user (`user.lang`) or falls back to the system default.
class VoiceSessionService
{
public:
  explicit VoiceSessionService(const VoiceEngineSeam& engines = {});

  void start(const drogon::WebSocketConnectionPtr& conn, int64_t userId,
             VoiceLang lang, const std::string& userName);
  void feedPcm(const drogon::WebSocketConnectionPtr& conn, const char* data,
               size_t len);
  void stop(const drogon::WebSocketConnectionPtr& conn);
  // Interrupts the assistant's current reply; the next user turn starts
  // cleanly (the session itself stays alive).
  void skip(const drogon::WebSocketConnectionPtr& conn);

private:
  struct Session
  {
    drogon::WebSocketConnectionPtr conn;
    VadService vad;
    NoiseSuppressor denoiser;
    bool denoise{true};
    /** RMS below which a batch skips the RNNoise path (silence gate). */
    float denoiseGateRms{0.0035F};
    int denoiseLogCounter{0};
    std::vector<ChatMessage> history;
    VoiceLang lang{VoiceLang::System};
    int64_t userId{0};
    bool nameKnown{false};
    std::atomic<bool> speaking{false};
    std::atomic<bool> interrupt{false};
    std::atomic<bool> active{true};
    std::thread worker;
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    std::vector<float> pcmQueue;
  };

  void workerLoop(std::shared_ptr<Session> session);
  void processTurn(Session& session, const std::vector<float>& samples);
  // Emits `voice:event` so the avatar reacts while the model is still
  // generating. Memory-derived signals (capture, recall) arrive once
  // MemoryService reaches this path; the wire contract does not change.
  Reaction emitReaction(Session& session, const ReactionSignals& signals);
  void speak(Session& session, const std::string& text);
  void sendJson(Session& session, const std::string& type,
                const Json::Value& payload) const;

  mutable std::mutex mutex_;
  std::unordered_map<const void*, std::shared_ptr<Session>> sessions_;
  ReactionEngine reactions_;
  IVoiceStt& stt_;
  IVoiceTts& tts_;
  IVoiceLlm& llm_;

  friend struct VoiceSessionTestAccess;
};

// System-wide voice language from config `stt.language`; the base for any
// interaction without a registered user (e.g. camera conversations).
VoiceLang voiceSystemLang();
