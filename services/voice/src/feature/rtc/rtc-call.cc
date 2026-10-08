#include "rtc-call.hxx"

#include <drogon/drogon.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <span>
#include <utility>

namespace
{

constexpr auto kControlTick = std::chrono::milliseconds(100);
constexpr auto kPlayoutTick = std::chrono::milliseconds(10);
constexpr auto kTailFlushAfter = std::chrono::milliseconds(40);
constexpr auto kDrainedAfter = std::chrono::milliseconds(600);
constexpr int kCaptureTimeoutMs = 1000;
constexpr int kFadeCaptureTimeoutMs = 100;
constexpr size_t kMaxEarlyClientMessages = 16;
constexpr size_t kStreamCapacityFrames = 50;

bool isMicrophone(const livekit::Track& track)
{
  if (track.kind() != livekit::TrackKind::KIND_AUDIO)
    return false;
  const auto source = track.source();
  return !source || *source == livekit::TrackSource::SOURCE_MICROPHONE ||
         *source == livekit::TrackSource::SOURCE_UNKNOWN;
}

}

RtcCall::RtcCall(RtcCallInput input)
    : sessions_(*input.sessions),
      join_(std::move(input.join)),
      url_(std::move(input.url)),
      timings_(input.timings),
      onJoined_(std::move(input.onJoined)),
      onEnded_(std::move(input.onEnded))
{
}

RtcCall::~RtcCall()
{
  requestStop(rtc_wire::DoneReason::Error);
  waitEnded();
  if (room_)
    room_->setDelegate(nullptr);
}

void RtcCall::run()
{
  control_ = std::thread([this] { controlLoop(); });
}

void RtcCall::requestStop(rtc_wire::DoneReason reason)
{
  {
    std::scoped_lock lock(mutex_);
    if (!stopReason_)
      stopReason_ = reason;
  }
  cv_.notify_all();
}

void RtcCall::farewell(const argus::voice::v1::RtcFarewell& request, std::function<void(bool)> done)
{
  if (!request.user_identity().empty() && request.user_identity() != join_.user_identity()) {
    done(false);
    return;
  }
  {
    std::unique_lock lock(mutex_);
    if (revoked_.exchange(true)) {
      if (!farewellFinished_) {
        farewellWaiters_.push_back(std::move(done));
        return;
      }
      lock.unlock();
      done(true);
      return;
    }
    if (stopReason_) {
      farewellFinished_ = true;
      lock.unlock();
      done(false);
      return;
    }
    farewell_ = FarewellRequest{.cause = request.reason(),
                                .done = std::move(done),
                                .deadline = std::chrono::steady_clock::now() + kFarewellBound};
  }
  cv_.notify_all();
}

void RtcCall::sayFarewell(const FarewellRequest& request)
{
  LOG_INFO << "Voice: RTC call " << join_.room() << " says goodbye (" << request.cause << ")";
  flushPlayout();
  publish(rtc_wire::revokedMessage(request.cause));
  const bool played = sessions_.farewell(*this, farewellReasonOf(request.cause));
  publishPending();
  wantState(played ? rtc_wire::AgentState::Speaking : rtc_wire::AgentState::Listening);
  applyState();
  if (played) {
    constexpr auto kTail = std::chrono::milliseconds(kSourceQueueMs + 120);
    std::optional<std::chrono::steady_clock::time_point> emptiedAt;
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= request.deadline)
        break;
      bool empty = false;
      {
        std::scoped_lock lock(playoutMutex_);
        empty = playout_.size() == 0;
      }
      if (empty && !emptiedAt)
        emptiedAt = now;
      if (emptiedAt && now - *emptiedAt >= kTail)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  std::vector<std::function<void(bool)>> waiters;
  {
    std::scoped_lock lock(mutex_);
    farewellFinished_ = true;
    waiters.swap(farewellWaiters_);
  }
  request.done(played);
  for (const auto& waiter : waiters)
    waiter(played);
}

void RtcCall::waitEnded()
{
  if (control_.joinable() && control_.get_id() != std::this_thread::get_id())
    control_.join();
}

bool RtcCall::connect()
{
  try {
    room_ = std::make_unique<livekit::Room>();
    room_->setDelegate(this);
    livekit::RoomOptions options;
    options.auto_subscribe = true;
    options.connect_timeout = timings_.connectTimeout;
    options.join_retries = 1;
    if (!room_->connect(url_, join_.agent_token(), options)) {
      LOG_WARN << "Voice: RTC connect to " << join_.room() << " failed";
      return false;
    }
    roomUp_.store(true);
    source_ = std::make_shared<livekit::AudioSource>(kSampleRate, 1, kSourceQueueMs);
    const auto local = room_->localParticipant().lock();
    if (!local)
      return false;
    track_ = local->publishAudioTrack(std::string(rtc_wire::kAgentTrackName), source_,
                                      livekit::TrackSource::SOURCE_MICROPHONE);
    if (!track_)
      return false;
    local->setAttributes({{std::string(rtc_wire::kAgentStateAttribute),
                           rtc_wire::agentStateToString(rtc_wire::AgentState::Initializing)}});
    std::scoped_lock lock(mutex_);
    startedAt_ = std::chrono::steady_clock::now();
    return true;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice: RTC join of " << join_.room() << " failed: " << error.what();
    return false;
  }
}

void RtcCall::controlLoop()
{
  const bool joined = connect();
  if (onJoined_)
    onJoined_(joined);
  if (!joined) {
    if (roomUp_.exchange(false))
      room_->disconnect();
    if (onEnded_)
      onEnded_({.room = join_.room(),
                .callId = join_.call_id(),
                .userId = userId(),
                .reason = rtc_wire::DoneReason::Error,
                .userJoined = false,
                .openingSpoken = false});
    ended_.store(true);
    return;
  }
  LOG_INFO << "Voice: RTC agent joined " << join_.room();
  playoutThread_ = std::thread([this] { playoutLoop(); });

  for (;;) {
    std::shared_ptr<livekit::Track> track;
    std::optional<rtc_wire::DoneReason> reason;
    std::optional<FarewellRequest> goodbye;
    {
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, kControlTick, [this] {
        return pendingTrack_ != nullptr || stopReason_.has_value() || !outbound_.empty() ||
               wanted_ != applied_ || farewell_.has_value();
      });
      track = std::exchange(pendingTrack_, nullptr);
      goodbye = std::exchange(farewell_, std::nullopt);
      const auto now = std::chrono::steady_clock::now();
      if (wanted_ == rtc_wire::AgentState::Thinking && now - thinkingSince_ > timings_.thinkingTimeout)
        wanted_ = rtc_wire::AgentState::Listening;
      reason = endReason(now);
    }
    if (goodbye) {
      sayFarewell(*goodbye);
      continue;
    }
    if (track && !reason && !revoked_.load()) {
      attachTrack(track);
      bool first = false;
      {
        std::scoped_lock lock(mutex_);
        first = !sessionStarted_;
      }
      if (first)
        startSession();
    }
    publishPending();
    applyState();
    if (reason) {
      finish(*reason);
      return;
    }
  }
}

std::optional<rtc_wire::DoneReason> RtcCall::endReason(std::chrono::steady_clock::time_point now) const
{
  if (stopReason_)
    return stopReason_;
  if (!userJoined_ && now - startedAt_ > timings_.firstJoinWait)
    return rtc_wire::DoneReason::Timeout;
  if (userJoined_ && !userPresent_ && now - userLeftAt_ > timings_.rejoinGrace)
    return rtc_wire::DoneReason::Timeout;
  return std::nullopt;
}

void RtcCall::attachTrack(const std::shared_ptr<livekit::Track>& track)
{
  detachReader();
  std::scoped_lock lock(readerMutex_);
  stream_ = livekit::AudioStream::fromTrack(track, {.capacity = kStreamCapacityFrames,
                                                   .noise_cancellation_module = {},
                                                   .noise_cancellation_options_json = {}});
  if (!stream_) {
    LOG_WARN << "Voice: RTC audio stream for " << join_.room() << " did not open";
    return;
  }
  resampler_.reset();
  reader_ = std::thread([this, stream = stream_] { readerLoop(stream); });
}

void RtcCall::detachReader()
{
  std::scoped_lock lock(readerMutex_);
  if (stream_)
    stream_->close();
  if (reader_.joinable())
    reader_.join();
  stream_.reset();
}

void RtcCall::readerLoop(const std::shared_ptr<livekit::AudioStream>& stream)
{
  livekit::AudioFrameEvent event;
  while (!stopping_.load() && stream->read(event))
    if (!revoked_.load())
      feedAudio(event.frame);
}

void RtcCall::feedAudio(const livekit::AudioFrame& frame)
{
  const int channels = std::max(1, frame.numChannels());
  const auto perChannel = static_cast<size_t>(frame.samplesPerChannel());
  const std::vector<int16_t>& data = frame.data();
  if (perChannel == 0 || data.size() < perChannel * static_cast<size_t>(channels))
    return;

  std::span<const int16_t> mono(data.data(), perChannel);
  if (channels > 1) {
    mono_.resize(perChannel);
    for (size_t i = 0; i < perChannel; ++i) {
      int sum = 0;
      for (int c = 0; c < channels; ++c)
        sum += data[i * static_cast<size_t>(channels) + static_cast<size_t>(c)];
      mono_[i] = static_cast<int16_t>(sum / channels);
    }
    mono = mono_;
  }

  std::span<const int16_t> pcm = mono;
  if (frame.sampleRate() != kSampleRate) {
    if (!resampler_ || resampler_->sourceRate() != frame.sampleRate())
      resampler_ = std::make_unique<AudioResampler>(
          AudioResamplerInput{.sourceRate = frame.sampleRate(), .targetRate = kSampleRate});
    resampler_->processInto(mono, resampled_);
    pcm = resampled_;
  }
  if (pcm.empty())
    return;
  samples_.resize(pcm.size());
  std::ranges::transform(pcm, samples_.begin(),
                         [](int16_t sample) { return static_cast<float>(sample) / 32768.0F; });
  sessions_.feedSamples(*this, samples_);
}

void RtcCall::startSession()
{
  argus::voice::v1::VoiceStart start;
  *start.mutable_identity() = join_.identity();
  start.set_mode(join_.mode());
  start.set_resume(join_.resume());
  start.set_opening_line(join_.opening_line());
  sessions_.start(*this, start);

  std::deque<rtc_wire::ClientMessage> early;
  {
    std::scoped_lock lock(mutex_);
    sessionStarted_ = true;
    early.swap(earlyClient_);
  }
  const bool speaksOpening = VoiceSessionService::openingWillBeSpoken(start);
  LOG_INFO << "Voice: RTC session started in " << join_.room() << " resume=" << join_.resume()
           << " speaks_opening=" << speaksOpening << " carried=" << !join_.opening_line().empty()
           << " early=" << early.size();
  wantState(rtc_wire::agentStateForOpening(speaksOpening));
  for (const auto& message : early)
    dispatch(message);
}

void RtcCall::finish(rtc_wire::DoneReason reason)
{
  LOG_INFO << "Voice: RTC call " << join_.room() << " ends ("
           << rtc_wire::doneReasonToString(reason) << ")";
  std::vector<std::function<void(bool)>> waiters;
  {
    std::scoped_lock lock(mutex_);
    farewellFinished_ = true;
    waiters.swap(farewellWaiters_);
    if (farewell_)
      waiters.push_back(std::move(farewell_->done));
    farewell_.reset();
  }
  for (const auto& waiter : waiters)
    waiter(false);
  sessions_.stop(*this);
  publishPending();
  if (roomUp_.load() && reason != rtc_wire::DoneReason::Revoked)
    publish(rtc_wire::doneMessage(reason));
  stopping_.store(true);
  playoutCv_.notify_all();
  if (playoutThread_.joinable())
    playoutThread_.join();
  detachReader();
  if (roomUp_.exchange(false)) {
    try {
      room_->disconnect();
    }
    catch (const std::exception& error) {
      LOG_WARN << "Voice: RTC disconnect of " << join_.room() << " failed: " << error.what();
    }
  }
  RtcCallReport report;
  {
    std::scoped_lock lock(mutex_);
    report = {.room = join_.room(),
              .callId = join_.call_id(),
              .userId = userId(),
              .reason = reason,
              .userJoined = userJoined_,
              .openingSpoken = openingSpoken_};
  }
  if (onEnded_)
    onEnded_(report);
  ended_.store(true);
}

bool RtcCall::connected() const
{
  return roomUp_.load() && !stopping_.load();
}

void RtcCall::sendServerFrame(argus::voice::v1::ServerFrame frame)
{
  switch (frame.body_case()) {
    case argus::voice::v1::ServerFrame::kTtsChunk:
      pushPlayout(frame.tts_chunk().pcm());
      wantState(rtc_wire::AgentState::Speaking);
      return;
    case argus::voice::v1::ServerFrame::kDone:
      return;
    case argus::voice::v1::ServerFrame::kInterrupted:
      flushPlayout();
      wantState(rtc_wire::AgentState::Listening);
      break;
    case argus::voice::v1::ServerFrame::kStt:
      if (frame.stt().final())
        wantState(rtc_wire::AgentState::Thinking);
      break;
    default:
      break;
  }
  auto message = rtc_wire::dataMessageOf(frame);
  if (!message)
    return;
  {
    std::scoped_lock lock(mutex_);
    if (outbound_.size() >= kMaxOutboundMessages) {
      LOG_WARN << "Voice: RTC data backlog in " << join_.room() << ", dropping " << message->topic;
      return;
    }
    outbound_.push_back(std::move(*message));
  }
  cv_.notify_all();
}

void RtcCall::pushPlayout(const std::string& pcm)
{
  const size_t count = pcm.size() / 2;
  if (count == 0)
    return;
  {
    std::scoped_lock lock(playoutMutex_);
    chunk_.resize(count);
    std::memcpy(chunk_.data(), pcm.data(), count * sizeof(int16_t));
    playout_.push(chunk_.data(), count);
    audible_ = true;
    lastChunkAt_ = std::chrono::steady_clock::now();
  }
  playoutCv_.notify_one();
}

void RtcCall::flushPlayout()
{
  {
    std::scoped_lock lock(playoutMutex_);
    if (fadeTail_.empty()) {
      const size_t fading = std::min(playout_.size(), kFadeSamples);
      fadeTail_.resize(fading);
      if (fading > 0)
        playout_.pop(fadeTail_.data(), fading);
      gain_.fadeOut(fadeTail_);
    }
    playout_.clear();
    flushPending_ = true;
    audible_ = false;
  }
  playoutCv_.notify_one();
}

void RtcCall::captureFade(std::span<const int16_t> fading)
{
  livekit::AudioFrame frame(std::vector<int16_t>(static_cast<size_t>(kFrameSamples), int16_t{0}),
                            kSampleRate, 1, kFrameSamples);
  for (size_t offset = 0; offset < fading.size(); offset += static_cast<size_t>(kFrameSamples)) {
    const size_t count = std::min(static_cast<size_t>(kFrameSamples), fading.size() - offset);
    std::fill(frame.data().begin(), frame.data().end(), int16_t{0});
    std::copy_n(fading.begin() + static_cast<std::ptrdiff_t>(offset), count, frame.data().begin());
    try {
      source_->captureFrame(frame, kFadeCaptureTimeoutMs);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Voice: RTC fade in " << join_.room() << " failed: " << error.what();
      return;
    }
  }
}

void RtcCall::duckPlayout(bool ducked)
{
  std::scoped_lock lock(playoutMutex_);
  gain_.duck(ducked);
}

void RtcCall::playoutLoop()
{
  livekit::AudioFrame frame(std::vector<int16_t>(kFrameSamples, 0), kSampleRate, 1, kFrameSamples);
  auto* samples = frame.data().data();
  std::vector<int16_t> fading;
  while (!stopping_.load()) {
    bool flush = false;
    bool have = false;
    bool drained = false;
    {
      std::unique_lock lock(playoutMutex_);
      playoutCv_.wait_for(lock, kPlayoutTick, [this] {
        return stopping_.load() || flushPending_ || playout_.size() >= static_cast<size_t>(kFrameSamples);
      });
      if (stopping_.load())
        break;
      flush = std::exchange(flushPending_, false);
      if (flush)
        fading.swap(fadeTail_);
      const auto now = std::chrono::steady_clock::now();
      if (!flush && playout_.size() >= static_cast<size_t>(kFrameSamples)) {
        playout_.pop(samples, kFrameSamples);
        gain_.apply({samples, static_cast<size_t>(kFrameSamples)});
        have = true;
      }
      else if (!flush && playout_.size() > 0 && now - lastChunkAt_ > kTailFlushAfter) {
        const size_t tail = playout_.size();
        playout_.pop(samples, tail);
        std::fill(samples + tail, samples + kFrameSamples, int16_t{0});
        gain_.apply({samples, static_cast<size_t>(kFrameSamples)});
        have = true;
      }
      else if (!flush && audible_ && playout_.size() == 0 && now - lastChunkAt_ > kDrainedAfter) {
        audible_ = false;
        drained = true;
      }
    }
    if (flush) {
      captureFade(fading);
      fading.clear();
    }
    if (have) {
      try {
        source_->captureFrame(frame, kCaptureTimeoutMs);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Voice: RTC capture in " << join_.room() << " failed: " << error.what();
      }
    }
    if (drained) {
      {
        std::scoped_lock lock(mutex_);
        openingSpoken_ = true;
      }
      wantState(rtc_wire::AgentState::Listening);
    }
  }
}

void RtcCall::wantState(rtc_wire::AgentState state)
{
  {
    std::scoped_lock lock(mutex_);
    if (wanted_ == state)
      return;
    wanted_ = state;
    if (state == rtc_wire::AgentState::Thinking)
      thinkingSince_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void RtcCall::applyState()
{
  rtc_wire::AgentState state{};
  {
    std::scoped_lock lock(mutex_);
    if (wanted_ == applied_)
      return;
    state = wanted_;
    applied_ = state;
  }
  try {
    if (const auto local = room_->localParticipant().lock())
      local->setAttributes({{std::string(rtc_wire::kAgentStateAttribute),
                             rtc_wire::agentStateToString(state)}});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice: RTC state update in " << join_.room() << " failed: " << error.what();
  }
}

void RtcCall::publishPending()
{
  std::deque<rtc_wire::DataMessage> pending;
  {
    std::scoped_lock lock(mutex_);
    pending.swap(outbound_);
  }
  for (const auto& message : pending)
    publish(message);
}

void RtcCall::publish(const rtc_wire::DataMessage& message)
{
  if (!roomUp_.load())
    return;
  try {
    const auto local = room_->localParticipant().lock();
    if (!local)
      return;
    const std::vector<uint8_t> payload(message.payload.begin(), message.payload.end());
    local->publishData(payload, true, {join_.user_identity()}, message.topic);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice: RTC data " << message.topic << " in " << join_.room()
             << " failed: " << error.what();
  }
}

void RtcCall::dispatch(const rtc_wire::ClientMessage& message)
{
  switch (message.kind) {
    case rtc_wire::ClientMessageKind::Context:
      sessions_.context(*this, message.context);
      break;
    case rtc_wire::ClientMessageKind::ActionResult:
      sessions_.actionResult(*this, message.actionResult);
      break;
    case rtc_wire::ClientMessageKind::Mute:
      sessions_.mute(*this, message.muted);
      break;
    case rtc_wire::ClientMessageKind::Skip:
      sessions_.skip(*this);
      break;
    case rtc_wire::ClientMessageKind::Hangup:
      requestStop(rtc_wire::DoneReason::Hangup);
      break;
    case rtc_wire::ClientMessageKind::Ignored:
      break;
  }
}

bool RtcCall::isUser(const livekit::RemoteParticipant* participant) const
{
  return participant != nullptr && participant->identity() == join_.user_identity();
}

void RtcCall::onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& event)
{
  if (!isUser(event.participant) || !event.track || !isMicrophone(*event.track))
    return;
  {
    std::scoped_lock lock(mutex_);
    pendingTrack_ = event.track;
    userPresent_ = true;
    userJoined_ = true;
  }
  cv_.notify_all();
}

void RtcCall::onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent& event)
{
  if (isUser(event.participant))
    LOG_INFO << "Voice: RTC user track left " << join_.room();
}

void RtcCall::onParticipantConnected(livekit::Room&, const livekit::ParticipantConnectedEvent& event)
{
  if (!isUser(event.participant))
    return;
  {
    std::scoped_lock lock(mutex_);
    userPresent_ = true;
  }
  cv_.notify_all();
}

void RtcCall::onParticipantDisconnected(livekit::Room&, const livekit::ParticipantDisconnectedEvent& event)
{
  if (!isUser(event.participant))
    return;
  {
    std::scoped_lock lock(mutex_);
    userPresent_ = false;
    userLeftAt_ = std::chrono::steady_clock::now();
    if (event.reason == livekit::DisconnectReason::ParticipantRemoved && !stopReason_)
      stopReason_ = rtc_wire::DoneReason::Revoked;
  }
  cv_.notify_all();
}

void RtcCall::onDisconnected(livekit::Room&, const livekit::DisconnectedEvent& event)
{
  roomUp_.store(false);
  LOG_INFO << "Voice: RTC room " << join_.room() << " disconnected the agent (reason "
           << static_cast<int>(event.reason) << ")";
  {
    std::scoped_lock lock(mutex_);
    const bool removed = event.reason == livekit::DisconnectReason::ParticipantRemoved ||
                         event.reason == livekit::DisconnectReason::RoomDeleted;
    if (!stopReason_)
      stopReason_ = !userJoined_ ? rtc_wire::DoneReason::Timeout
                    : removed    ? rtc_wire::DoneReason::Revoked
                                 : rtc_wire::DoneReason::Error;
  }
  cv_.notify_all();
}

void RtcCall::onUserPacketReceived(livekit::Room&, const livekit::UserDataPacketEvent& event)
{
  if (revoked_.load())
    return;
  rtc_wire::ClientMessage message =
      rtc_wire::clientMessageOf({.topic = event.topic, .payload = event.data});
  if (message.kind == rtc_wire::ClientMessageKind::Ignored)
    return;
  {
    std::scoped_lock lock(mutex_);
    const bool unknownSender = event.participant == nullptr && !sessionStarted_;
    if (!isUser(event.participant) && !unknownSender)
      return;
    if (!sessionStarted_ && message.kind != rtc_wire::ClientMessageKind::Hangup) {
      if (earlyClient_.size() < kMaxEarlyClientMessages)
        earlyClient_.push_back(std::move(message));
      return;
    }
  }
  dispatch(message);
}
