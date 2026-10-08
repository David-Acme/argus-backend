#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <feature/voice/call-history.hxx>
#include <feature/voice/farewell-lines.hxx>
#include <feature/voice/turn-transcript.hxx>
#include <feature/voice/voice-engine-seam.hxx>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <llm/llm-service.hxx>
#include <feature/voice/services/noise/noise-suppression-service.hxx>
#include <feature/voice/services/reaction/reaction-engine.hxx>
#include <tts/tts-wire.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <audio/audio-resampler.hxx>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
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
  virtual void duckPlayout(bool) {}
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
  void feedSamples(VoiceSessionSink& sink, std::span<const float> samples);
  bool announce(int64_t userId, const std::string& text);
  bool farewell(VoiceSessionSink& sink, FarewellReason reason);
  void warmFarewells();
  [[nodiscard]] FarewellAudio farewellAudio(const FarewellKey& key) const;
  [[nodiscard]] static VoiceLang langOf(const argus::voice::v1::VoiceIdentity& identity);
  [[nodiscard]] static bool openingWillBeSpoken(const argus::voice::v1::VoiceStart& request);
  void stop(VoiceSessionSink& sink);
  void skip(VoiceSessionSink& sink);
  void context(VoiceSessionSink& sink, const argus::voice::v1::VoiceContext& context);
  void actionResult(VoiceSessionSink& sink, const argus::voice::v1::VoiceActionResult& result);
  void mute(VoiceSessionSink& sink, bool muted);

private:
  struct CameraNotice
  {
    std::string camera;
    std::string summary;
    std::chrono::steady_clock::time_point at{};
  };

  struct ActionFailure
  {
    std::string name;
    std::string detail;
    std::chrono::steady_clock::time_point at{};
  };

  struct Notice
  {
    std::string spoken;
    std::string event;
    std::string camera;
  };

  struct Announcement
  {
    std::string text;
    std::chrono::steady_clock::time_point at{};
  };

  struct CameraOffer
  {
    std::string camera;
    std::chrono::steady_clock::time_point at{};
  };

  struct SpeakerProbe
  {
    std::mutex mutex;
    std::condition_variable done;
    bool finished{false};
    std::optional<VoiceSpeaker> speaker;
  };

  struct SpeakOutcome
  {
    bool audible{false};
    bool interrupted{false};
    std::chrono::steady_clock::time_point firstAudioAt{};
  };

  struct TurnClock
  {
    using Stamp = std::atomic<std::chrono::steady_clock::time_point>;

    static constexpr std::chrono::steady_clock::time_point none()
    {
      return std::chrono::steady_clock::time_point{};
    }

    void reset(std::chrono::steady_clock::time_point detectedAt)
    {
      detected.store(detectedAt);
      transcribed.store(none());
      firstToken.store(none());
      firstAudio.store(none());
      requested.store(none());
      firstSentence.store(none());
      firstChunk.store(none());
    }

    static void stamp(Stamp& mark, std::chrono::steady_clock::time_point now)
    {
      auto expected = none();
      std::ignore = mark.compare_exchange_strong(expected, now, std::memory_order_relaxed);
    }

    Stamp detected{none()};
    Stamp transcribed{none()};
    Stamp firstToken{none()};
    Stamp firstAudio{none()};
    Stamp requested{none()};
    Stamp firstSentence{none()};
    Stamp firstChunk{none()};
  };

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

  struct SessionInit
  {
    std::unique_ptr<VadModel> model;
    VoiceLang lang{VoiceLang::System};
  };

  struct Session
  {
    explicit Session(SessionInit init)
        : vad(std::move(init.model)), history(init.lang), lang(init.lang)
    {
    }

    static constexpr size_t kPcmRingSamples = static_cast<size_t>(16000) * 30;

    VoiceSessionSink* sink{nullptr};
    VadService vad;
    NoiseSuppressor denoiser;
    bool denoiseBypassed{false};
    std::vector<float> batch;
    std::vector<float> cleaned;
    bool denoise{true};
    float denoiseGateRms{0.0035F};
    int denoiseLogCounter{0};
    CallHistory history;
    VoiceLang lang{VoiceLang::System};
    std::string callId;
    int64_t userId{0};
    std::string role;
    std::string userName;
    std::atomic<bool> speaking{false};
    bool ducked{false};
    std::atomic<bool> interrupt{false};
    std::atomic<bool> active{true};
    std::atomic<bool> muted{false};
    std::atomic<bool> vadResetPending{false};
    std::atomic<bool> farewell{false};
    std::mutex turnMutex;
    std::stop_source turnStop;
    std::stop_source callStop;
    std::thread worker;
    std::thread primeThread;
    std::mutex pcmMutex;
    std::condition_variable pcmCv;
    SampleRing pcmRing{kPcmRingSamples};
    bool duplex{false};
    bool traceLatency{false};
    TurnClock turnClock;
    std::atomic<bool> turnClockLive{false};
    std::chrono::milliseconds bargeGuard{300};
    std::thread turnThread;
    std::mutex duplexMutex;
    DuplexTurn turn;
    std::mutex noticeMutex;
    std::vector<std::string> pendingNotes;
    std::optional<std::string> pendingSituation;
    std::optional<CameraNotice> pendingCamera;
    std::deque<ActionFailure> pendingFailures;
    std::deque<Announcement> pendingAnnouncements;
    std::deque<std::pair<int64_t, std::string>> sentActions;
    std::chrono::steady_clock::time_point lastNoticeAt{};
    std::atomic<int64_t> actionSeq{0};
    std::optional<CameraOffer> offer;
    std::shared_ptr<SpeakerProbe> speakerProbe;
    std::thread speakerThread;
    VoiceSpeakerVerdict speakerVerdict{VoiceSpeakerVerdict::Unknown};
    VoiceSpeakerVerdict speakerStreakVerdict{VoiceSpeakerVerdict::Unknown};
    VoiceSpeakerVerdict appliedSpeakerVerdict{VoiceSpeakerVerdict::Unknown};
    int speakerStreak{0};
    std::string deviceHash;
    std::string callKey;
    bool speakerHeard{false};
    std::shared_ptr<TurnTranscript> listening;
  };

  struct HeardTurn
  {
    const std::vector<float>& samples;
    std::shared_ptr<TurnTranscript> transcript;
    std::chrono::steady_clock::time_point detected{};
  };

  std::shared_ptr<Session> sessionOf(VoiceSessionSink& sink) const;
  void workerLoop(const std::shared_ptr<Session>& session);
  void duplexLoop(const std::shared_ptr<Session>& session);
  static bool nextBatch(Session& session);
  static std::span<const float> cleanBatch(Session& session);
  static void resetListening(Session& session);
  void launchTurn(const std::shared_ptr<Session>& session,
                  std::function<void(Session&)> body);
  ListenState listenState(Session& session);
  void bargeIn(Session& session);
  void followDuck(Session& session, bool ducked) const;
  bool sendDuplexChunk(Session& session, argus::voice::v1::ServerFrame frame);
  bool sendDuplexAssistant(Session& session, argus::voice::v1::ServerFrame frame);
  void processTurn(Session& session, const std::vector<float>& samples);
  void processTurn(Session& session, const HeardTurn& heard);
  void followUtterance(Session& session);
  std::shared_ptr<TurnTranscript> takeTranscript(Session& session);
  struct Transcript
  {
    std::string text;
    bool streamed{false};
  };
  Transcript transcribe(Session& session, const HeardTurn& heard);
  static ChatRequest turnRequest(Session& session);
  bool answerOffer(Session& session, const std::string& userText);
  std::shared_ptr<SpeakerProbe> probeSpeaker(Session& session, const std::vector<float>& samples);
  static std::optional<VoiceSpeaker> awaitSpeaker(const std::shared_ptr<SpeakerProbe>& probe);
  static void observeSpeaker(Session& session, const std::optional<VoiceSpeaker>& speaker);
  void primeLlm(Session& session);
  void applyNotes(Session& session);
  std::optional<Notice> takeNotice(Session& session);
  void deliverNotice(Session& session, const Notice& notice);
  void rememberAction(Session& session, const ClientAction& action);
  Reaction emitReaction(Session& session, const ReactionSignals& signals);
  SpeakOutcome speak(Session& session, const std::string& text);
  void sendFrame(Session& session, argus::voice::v1::ServerFrame frame) const;

  mutable std::mutex mutex_;
  std::unordered_map<VoiceSessionSink*, std::shared_ptr<Session>> sessions_;
  ReactionEngine reactions_;
  IVoiceStt& stt_;
  IVoiceTts& tts_;
  IVoiceLlm& llm_;
  IVoiceIdentity& identity_;
  IVoiceVad& vad_;
  IVoiceSpeaker& speaker_;
  FarewellCache farewells_;

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
