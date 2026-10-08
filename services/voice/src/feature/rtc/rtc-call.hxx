#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <feature/rtc/playout-gain.hxx>
#include <feature/rtc/rtc-wire.hxx>
#include <feature/voice/voice-session-service.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>

#include <audio/audio-resampler.hxx>
#include <livekit/livekit.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct RtcCallTimings
{
  std::chrono::milliseconds rejoinGrace{20000};
  std::chrono::milliseconds firstJoinWait{60000};
  std::chrono::milliseconds thinkingTimeout{20000};
  std::chrono::milliseconds connectTimeout{5000};
};

struct RtcCallReport
{
  std::string room;
  std::string callId;
  int64_t userId{0};
  rtc_wire::DoneReason reason{rtc_wire::DoneReason::Hangup};
  bool userJoined{false};
  bool openingSpoken{false};
};

struct RtcCallInput
{
  VoiceSessionService* sessions{nullptr};
  argus::voice::v1::RtcJoin join;
  std::string url;
  RtcCallTimings timings;
  std::function<void(bool)> onJoined;
  std::function<void(const RtcCallReport&)> onEnded;
};

class RtcCall final : public VoiceSessionSink, public livekit::RoomDelegate
{
public:
  explicit RtcCall(RtcCallInput input);
  RtcCall(const RtcCall&) = delete;
  RtcCall& operator=(const RtcCall&) = delete;
  RtcCall(RtcCall&&) = delete;
  RtcCall& operator=(RtcCall&&) = delete;
  ~RtcCall() override;

  void run();
  void requestStop(rtc_wire::DoneReason reason);
  void farewell(const argus::voice::v1::RtcFarewell& request, std::function<void(bool)> done);
  void waitEnded();

  [[nodiscard]] const std::string& room() const { return join_.room(); }
  [[nodiscard]] int64_t userId() const { return join_.identity().user_id(); }
  [[nodiscard]] bool ended() const { return ended_.load(); }

  bool connected() const override;
  void sendServerFrame(argus::voice::v1::ServerFrame frame) override;
  void duckPlayout(bool ducked) override;

  void onTrackSubscribed(livekit::Room& room, const livekit::TrackSubscribedEvent& event) override;
  void onTrackUnsubscribed(livekit::Room& room, const livekit::TrackUnsubscribedEvent& event) override;
  void onParticipantConnected(livekit::Room& room, const livekit::ParticipantConnectedEvent& event) override;
  void onParticipantDisconnected(livekit::Room& room, const livekit::ParticipantDisconnectedEvent& event) override;
  void onDisconnected(livekit::Room& room, const livekit::DisconnectedEvent& event) override;
  void onUserPacketReceived(livekit::Room& room, const livekit::UserDataPacketEvent& event) override;

  static constexpr int kSampleRate = 16000;
  static constexpr int kFrameSamples = kSampleRate / 100;
  static constexpr int kSourceQueueMs = 40;
  static constexpr size_t kFadeSamples = static_cast<size_t>(kSampleRate) * 60 / 1000;
  static constexpr size_t kPlayoutCapacity = static_cast<size_t>(kSampleRate) * 120;
  static constexpr size_t kMaxOutboundMessages = 256;
  static constexpr auto kFarewellBound = std::chrono::milliseconds(2300);

private:
  struct FarewellRequest
  {
    std::string cause;
    std::function<void(bool)> done;
    std::chrono::steady_clock::time_point deadline{};
  };

  void sayFarewell(const FarewellRequest& request);
  bool connect();
  void controlLoop();
  void dispatch(const rtc_wire::ClientMessage& message);
  [[nodiscard]] bool isUser(const livekit::RemoteParticipant* participant) const;
  void readerLoop(const std::shared_ptr<livekit::AudioStream>& stream);
  void playoutLoop();
  void attachTrack(const std::shared_ptr<livekit::Track>& track);
  void detachReader();
  void startSession();
  void finish(rtc_wire::DoneReason reason);
  void publishPending();
  void publish(const rtc_wire::DataMessage& message);
  void applyState();
  void wantState(rtc_wire::AgentState state);
  void feedAudio(const livekit::AudioFrame& frame);
  void pushPlayout(const std::string& pcm);
  void flushPlayout();
  void captureFade(std::span<const int16_t> fading);
  [[nodiscard]] std::optional<rtc_wire::DoneReason> endReason(std::chrono::steady_clock::time_point now) const;

  VoiceSessionService& sessions_;
  argus::voice::v1::RtcJoin join_;
  std::string url_;
  RtcCallTimings timings_;
  std::function<void(bool)> onJoined_;
  std::function<void(const RtcCallReport&)> onEnded_;

  std::unique_ptr<livekit::Room> room_;
  std::shared_ptr<livekit::AudioSource> source_;
  std::shared_ptr<livekit::LocalAudioTrack> track_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::shared_ptr<livekit::Track> pendingTrack_;
  bool userPresent_{false};
  bool userJoined_{false};
  std::chrono::steady_clock::time_point userLeftAt_{};
  std::chrono::steady_clock::time_point startedAt_{};
  std::optional<rtc_wire::DoneReason> stopReason_;
  std::deque<rtc_wire::DataMessage> outbound_;
  std::deque<rtc_wire::ClientMessage> earlyClient_;
  std::optional<FarewellRequest> farewell_;
  std::vector<std::function<void(bool)>> farewellWaiters_;
  bool farewellFinished_{false};
  rtc_wire::AgentState wanted_{rtc_wire::AgentState::Initializing};
  rtc_wire::AgentState applied_{rtc_wire::AgentState::Initializing};
  std::chrono::steady_clock::time_point thinkingSince_{};
  bool sessionStarted_{false};
  bool openingSpoken_{false};

  std::mutex readerMutex_;
  std::shared_ptr<livekit::AudioStream> stream_;
  std::thread reader_;
  std::unique_ptr<AudioResampler> resampler_;
  std::vector<int16_t> mono_;
  std::vector<int16_t> resampled_;
  std::vector<float> samples_;

  std::mutex playoutMutex_;
  std::condition_variable playoutCv_;
  BasicSampleRing<int16_t> playout_{kPlayoutCapacity};
  std::vector<int16_t> chunk_;
  std::vector<int16_t> fadeTail_;
  PlayoutGain gain_;
  bool flushPending_{false};
  bool audible_{false};
  std::chrono::steady_clock::time_point lastChunkAt_{};
  std::thread playoutThread_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> revoked_{false};
  std::atomic<bool> roomUp_{false};
  std::atomic<bool> ended_{false};
  std::thread control_;
};
