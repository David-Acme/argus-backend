#include "camera-conversation.hxx"

#include <feature/watch/wake-words.hxx>

#include <audio/audio-resampler.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <utility>

namespace
{

constexpr auto kTick = std::chrono::milliseconds(100);
constexpr size_t kMaxReplySamples = static_cast<size_t>(kWatchRate) * 60;
constexpr int kVadWindowMs = 32;

argus::voice::v1::VoiceContext noteOf(const std::string& text)
{
  argus::voice::v1::VoiceContext context;
  context.set_kind(argus::voice::v1::VOICE_CONTEXT_NOTE);
  context.set_text(text);
  return context;
}

}

std::vector<int16_t> pcm16Of(const std::string& bytes)
{
  std::vector<int16_t> samples(bytes.size() / 2);
  for (size_t index = 0; index < samples.size(); ++index) {
    const auto low = static_cast<uint16_t>(static_cast<unsigned char>(bytes[2 * index]));
    const auto high = static_cast<uint16_t>(static_cast<unsigned char>(bytes[(2 * index) + 1]));
    samples[index] = static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U)));
  }
  return samples;
}

std::vector<float> floatsOf(std::span<const int16_t> samples)
{
  std::vector<float> out(samples.size());
  std::ranges::transform(samples, out.begin(), [](int16_t sample) { return static_cast<float>(sample) / 32768.0F; });
  return out;
}

int64_t steadyMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

CameraConversation::CameraConversation(CameraConversationInput input)
    : input_(std::move(input)), cameraId_(input_.cameraId)
{
  snapshot_.conversationId = input_.id;
  snapshot_.userId = input_.start.identity().user_id();
  snapshot_.cameraId = input_.cameraId;
  snapshot_.cameraName = input_.cameraName;
  snapshot_.audience = input_.audience;
  snapshot_.startedAt = epochSeconds();
  snapshot_.updatedAt = snapshot_.startedAt;
}

CameraConversation::~CameraConversation()
{
  requestEnd(ConversationEnd::Shutdown);
  join();
  close(ConversationEnd::Shutdown);
  finishSpeaker(true);
}

void CameraConversation::join()
{
  if (ticker_.joinable() && ticker_.get_id() != std::this_thread::get_id()) {
    ticker_.request_stop();
    ticker_.join();
  }
}

void CameraConversation::open(const std::vector<float>& initialAudio)
{
  const int64_t now = steadyMs();
  {
    std::scoped_lock lock(mutex_);
    openedAt_ = now;
    lastFrameAt_ = now;
    lastSpeechAt_ = now;
  }
  openSpeaker(true);
  const VoiceWatchConfig& config = input_.config;
  input_.sessions->start(
      *this, input_.start,
      VoiceSessionOptions{.surface = input_.surface,
                          .minSilenceFrames = static_cast<int>(config.endSilence.count() / kVadWindowMs),
                          .maxTurnFrames = static_cast<int>(config.maxTurn.count() / kVadWindowMs),
                          .askAddressed = false,
                          .incompleteHold = config.incompleteHold - config.endSilence,
                          .incomplete = [](std::string_view text) { return trailsOff(text); },
                          .carry = std::nullopt});
  input_.sessions->context(*this, noteOf(cameraNote(input_.lang, input_.cameraName)));
  for (const std::string& note : input_.notes)
    input_.sessions->context(*this, noteOf(note));
  started_.store(true);
  if (!initialAudio.empty())
    input_.sessions->feedSamples(*this, initialAudio);
  setState(ConversationState::Listening);
  ticker_ = std::jthread([this](const std::stop_token& stop) { tick(stop); });
  LOG_INFO << "Voice watch: conversation " << input_.id << " opened on camera " << input_.cameraId;
}

void CameraConversation::feed(const EarFrame& frame)
{
  if (frame.cameraId != cameraId_.load() || closed_.load() || !started_.load())
    return;
  const int64_t now = steadyMs();
  {
    std::scoped_lock lock(mutex_);
    lastFrameAt_ = now;
    if (frame.speech)
      lastSpeechAt_ = now;
  }
  if (input_.echo && input_.echo->muted(frame.cameraId, frame.at))
    return;
  input_.sessions->feedSamples(*this, frame.samples);
}

void CameraConversation::bargeIn(const std::vector<float>& heard)
{
  if (closed_.load())
    return;
  LOG_INFO << "Voice watch: conversation " << input_.id << " is interrupted by its speaker";
  input_.sessions->skip(*this);
  std::shared_ptr<WatchSpeakerStream> speaker;
  {
    std::scoped_lock lock(mutex_);
    speaker = speaker_;
    replyPcm_.clear();
  }
  if (speaker)
    speaker->cancel();
  if (input_.echo)
    input_.echo->endPlayback(cameraId_.load(), WatchClock::now());
  if (!heard.empty())
    input_.sessions->feedSamples(*this, heard);
}

void CameraConversation::requestEnd(ConversationEnd reason)
{
  std::scoped_lock lock(mutex_);
  if (requested_ == ConversationEnd::None)
    requested_ = reason;
}

std::optional<VoiceSessionCarry> CameraConversation::handOver(std::chrono::milliseconds wait)
{
  {
    std::scoped_lock lock(mutex_);
    deviceHandoff_ = true;
    if (requested_ == ConversationEnd::None)
      requested_ = ConversationEnd::Handoff;
  }
  const auto until = std::chrono::steady_clock::now() + wait;
  while (!closed_.load() && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::scoped_lock lock(mutex_);
  return std::exchange(carry_, std::nullopt);
}

void CameraConversation::markHandoff(ConversationHandoff handoff)
{
  {
    std::scoped_lock lock(mutex_);
    snapshot_.handoff = handoff;
    snapshot_.updatedAt = epochSeconds();
  }
  publish();
}

ConversationSnapshot CameraConversation::snapshot() const
{
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

bool CameraConversation::speaking() const
{
  std::scoped_lock lock(mutex_);
  return speaker_ != nullptr;
}

bool CameraConversation::connected() const
{
  return !closed_.load();
}

std::string CameraConversation::shapeSpeech(const std::string& text) const
{
  return withoutSelfName(text, input_.lang);
}

bool CameraConversation::admitsTurn(const std::string& text, VoiceSpeakerVerdict verdict)
{
  const bool holder = verdict == VoiceSpeakerVerdict::Holder || verdict == VoiceSpeakerVerdict::Unknown;
  std::scoped_lock lock(mutex_);
  if (holder && delegationPhrase(text)) {
    delegated_ = true;
    return true;
  }
  if (holder || userId() <= 0 || delegated_ || mentionsWake({.text = text, .maxDistance = input_.config.wakeMaxDistance}))
    return true;
  userTurnAt_ = 0;
  if (!snapshot_.turns.empty() && snapshot_.turns.back().role == "user")
    snapshot_.turns.pop_back();
  return false;
}

void CameraConversation::sendServerFrame(argus::voice::v1::ServerFrame frame)
{
  if (closed_.load())
    return;
  const int64_t now = steadyMs();
  switch (frame.body_case()) {
    case argus::voice::v1::ServerFrame::kTtsChunk: {
      const std::vector<int16_t> pcm = pcm16Of(frame.tts_chunk().pcm());
      if (pcm.empty())
        return;
      bool needOpen = false;
      {
        std::scoped_lock lock(mutex_);
        needOpen = speaker_ == nullptr || speakerEnding_;
        lastChunkAt_ = now;
        if (userTurnAt_ > 0 && firstAudioAt_ < userTurnAt_)
          firstAudioAt_ = now;
        if (replyPcm_.size() + pcm.size() <= kMaxReplySamples)
          replyPcm_.insert(replyPcm_.end(), pcm.begin(), pcm.end());
      }
      if (needOpen) {
        finishSpeaker(false);
        openSpeaker(false);
      }
      setState(ConversationState::Speaking);
      writeSpeaker(pcm);
      return;
    }
    case argus::voice::v1::ServerFrame::kAssistant:
      if (!frame.assistant().text().empty())
        addTurn("argus", frame.assistant().text());
      return;
    case argus::voice::v1::ServerFrame::kStt: {
      if (!frame.stt().final() || frame.stt().text().empty())
        return;
      const std::string text = frame.stt().text();
      {
        std::scoped_lock lock(mutex_);
        userTurnAt_ = now;
        acked_ = false;
        replyPcm_.clear();
        if (closingPhrase(text))
          closeAfterReply_ = true;
      }
      addTurn("user", text);
      setState(ConversationState::Thinking);
      if (input_.onUserText)
        input_.onUserText(text);
      return;
    }
    case argus::voice::v1::ServerFrame::kAction:
      LOG_INFO << "Voice watch: conversation " << input_.id << " has no screen for " << frame.action().name();
      return;
    default:
      return;
  }
}

void CameraConversation::openSpeaker(bool earcon)
{
  if (!input_.camera)
    return;
  auto stream = input_.camera->speak({.cameraId = cameraId_.load(),
                                      .sampleRate = kWatchRate,
                                      .earcon = earcon,
                                      .conversationId = input_.id,
                                      .onEvent = [this](const argus::camera::v1::SpeakEvent& event) {
                                        onSpeakerEvent(event);
                                      }});
  const int64_t now = steadyMs();
  std::scoped_lock lock(mutex_);
  if (!stream) {
    speakerLost_ = true;
    return;
  }
  speaker_ = std::shared_ptr<WatchSpeakerStream>(std::move(stream));
  speakerEnding_ = false;
  speakerDone_ = false;
  lastChunkAt_ = now;
  if (input_.echo)
    input_.echo->beginPlayback(cameraId_.load(), WatchClock::now());
}

void CameraConversation::writeSpeaker(std::span<const int16_t> pcm)
{
  std::shared_ptr<WatchSpeakerStream> speaker;
  {
    std::scoped_lock lock(mutex_);
    speaker = speaker_;
  }
  if (!speaker)
    return;
  if (!speaker->write(pcm)) {
    std::scoped_lock lock(mutex_);
    speakerLost_ = true;
    return;
  }
  if (input_.echo)
    input_.echo->addPlayed(cameraId_.load(), floatsOf(pcm), WatchClock::now());
}

void CameraConversation::onSpeakerEvent(const argus::camera::v1::SpeakEvent& event)
{
  namespace v1 = argus::camera::v1;
  switch (event.state()) {
    case v1::SPEAK_STATE_DONE: {
      const int64_t remaining =
          std::max<int64_t>(0, event.playback_end_ms() -
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
      if (input_.echo)
        input_.echo->endPlayback(cameraId_.load(), WatchClock::now() + std::chrono::milliseconds(remaining));
      std::scoped_lock lock(mutex_);
      speakerDone_ = true;
      return;
    }
    case v1::SPEAK_STATE_BUSY: {
      std::scoped_lock lock(mutex_);
      speakerBusy_ = true;
      return;
    }
    case v1::SPEAK_STATE_REFUSED:
    case v1::SPEAK_STATE_LINE_LOST: {
      LOG_WARN << "Voice watch: conversation " << input_.id << " lost the speaker of camera " << cameraId_.load()
               << " (" << event.detail() << ")";
      std::scoped_lock lock(mutex_);
      speakerLost_ = true;
      return;
    }
    default:
      return;
  }
}

void CameraConversation::finishSpeaker(bool cancel)
{
  std::shared_ptr<WatchSpeakerStream> speaker;
  {
    std::scoped_lock lock(mutex_);
    speaker = std::exchange(speaker_, nullptr);
    speakerEnding_ = false;
    speakerDone_ = false;
  }
  if (!speaker)
    return;
  if (cancel)
    speaker->cancel();
  else
    speaker->end();
  static_cast<void>(speaker->finish());
  if (cancel && input_.echo)
    input_.echo->endPlayback(cameraId_.load(), WatchClock::now());
}

bool CameraConversation::hop()
{
  if (!input_.nextCamera)
    return false;
  const auto next = input_.nextCamera(cameraId_.load());
  if (!next)
    return false;
  finishSpeaker(true);
  LOG_INFO << "Voice watch: conversation " << input_.id << " moves from camera " << cameraId_.load() << " to "
           << next->cameraId;
  cameraId_.store(next->cameraId);
  std::vector<int16_t> replay;
  {
    std::scoped_lock lock(mutex_);
    snapshot_.cameraId = next->cameraId;
    snapshot_.cameraName = next->name;
    speakerBusy_ = false;
    speakerLost_ = false;
    lastFrameAt_ = steadyMs();
    replay = replyPcm_;
  }
  publish();
  if (!replay.empty()) {
    openSpeaker(false);
    writeSpeaker(replay);
  }
  return true;
}

void CameraConversation::sayDirect(const std::string& line)
{
  if (input_.tts == nullptr || line.empty())
    return;
  const std::string spoken = withoutSelfName(line, input_.lang);
  int sourceRate = kWatchRate;
  TtsRequest request;
  request.text = spoken;
  request.lang = input_.lang == VoiceLang::En ? TtsLang::EN : TtsLang::ES;
  request.quality = TtsQuality::Auto;
  try {
    request.speed = input_.tts->defaultSpeed();
    sourceRate = input_.tts->sampleRate();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice watch: TTS unavailable for a camera line: " << error.what();
    return;
  }
  AudioResampler resampler({.sourceRate = sourceRate > 0 ? sourceRate : kWatchRate, .targetRate = kWatchRate});
  bool opened = false;
  try {
    input_.tts->synthesizeStream({.request = request,
                                  .onChunk =
                                      [&](const std::vector<float>& chunk) {
                                        std::vector<int16_t> raw(chunk.size());
                                        std::ranges::transform(chunk, raw.begin(), [](float sample) {
                                          return static_cast<int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
                                        });
                                        const std::vector<int16_t> pcm = resampler.process(raw.data(), raw.size());
                                        if (pcm.empty())
                                          return;
                                        if (!opened) {
                                          bool needOpen = false;
                                          {
                                            std::scoped_lock lock(mutex_);
                                            needOpen = speaker_ == nullptr || speakerEnding_;
                                          }
                                          if (needOpen) {
                                            finishSpeaker(false);
                                            openSpeaker(false);
                                          }
                                          opened = true;
                                        }
                                        {
                                          std::scoped_lock lock(mutex_);
                                          lastChunkAt_ = steadyMs();
                                        }
                                        writeSpeaker(pcm);
                                      },
                                  .cancellation = {}});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Voice watch: a camera line was not synthesised: " << error.what();
  }
  addTurn("argus", spoken);
}

void CameraConversation::tick(const std::stop_token& stop)
{
  std::mutex waitMutex;
  std::condition_variable_any wake;
  const VoiceWatchConfig& config = input_.config;
  while (!stop.stop_requested() && !closed_.load()) {
    {
      std::unique_lock lock(waitMutex);
      static_cast<void>(wake.wait_for(lock, stop, kTick, [] { return false; }));
    }
    if (stop.stop_requested())
      break;
    const int64_t now = steadyMs();
    bool endSpeaker = false;
    bool done = false;
    bool busy = false;
    bool lost = false;
    bool closeAfter = false;
    ConversationEnd requested = ConversationEnd::None;
    ReplyNudge nudge = ReplyNudge::None;
    ConversationEnd due = ConversationEnd::None;
    {
      std::scoped_lock lock(mutex_);
      endSpeaker = speaker_ != nullptr && !speakerEnding_ && now - lastChunkAt_ >= config.replyGap.count();
      if (endSpeaker)
        speakerEnding_ = true;
      done = speakerDone_;
      busy = std::exchange(speakerBusy_, false);
      lost = std::exchange(speakerLost_, false);
      lost = lost || (now - lastFrameAt_ >= config.cameraDrop.count() && speaker_ == nullptr &&
                      now - openedAt_ >= config.cameraDrop.count());
      closeAfter = closeAfterReply_;
      requested = requested_;
      nudge = replyNudge({.now = now,
                          .userTurnAt = userTurnAt_,
                          .firstAudioAt = firstAudioAt_,
                          .acked = acked_,
                          .ackAfterMs = config.ackAfter.count(),
                          .failAfterMs = config.failAfter.count()});
      if (nudge == ReplyNudge::Acknowledge)
        acked_ = true;
      if (nudge == ReplyNudge::GiveUp)
        userTurnAt_ = 0;
      due = conversationDue({.now = now,
                             .lastSpeech = lastSpeechAt_,
                             .startedAt = openedAt_,
                             .idleCloseMs = config.idleClose.count(),
                             .maxLengthMs = config.maxLength.count(),
                             .busy = speaker_ != nullptr || userTurnAt_ > 0});
    }
    if (requested != ConversationEnd::None) {
      close(requested);
      return;
    }
    if (busy || lost) {
      if (hop())
        continue;
      close(busy ? ConversationEnd::BusyLine : ConversationEnd::CameraLost);
      return;
    }
    if (endSpeaker) {
      std::shared_ptr<WatchSpeakerStream> speaker;
      {
        std::scoped_lock lock(mutex_);
        speaker = speaker_;
      }
      if (speaker)
        speaker->end();
    }
    if (done) {
      finishSpeaker(false);
      if (closeAfter) {
        close(ConversationEnd::Closing);
        return;
      }
      setState(ConversationState::Listening);
    }
    if (nudge == ReplyNudge::Acknowledge)
      sayDirect(acknowledgementLine(input_.lang));
    if (nudge == ReplyNudge::GiveUp) {
      input_.sessions->skip(*this);
      sayDirect(giveUpLine(input_.lang));
    }
    if (due != ConversationEnd::None) {
      close(due);
      return;
    }
  }
}

void CameraConversation::close(ConversationEnd reason)
{
  bool device = false;
  {
    std::scoped_lock lock(mutex_);
    device = deviceHandoff_;
  }
  if (reason == ConversationEnd::Handoff && !device && !closed_.load())
    sayDirect(handoffLine(input_.lang));
  if (closed_.exchange(true))
    return;
  finishSpeaker(reason != ConversationEnd::Closing && reason != ConversationEnd::Silence &&
                reason != ConversationEnd::Handoff);
  std::optional<VoiceSessionCarry> carry;
  if (started_.load())
    carry = input_.sessions->stopWithCarry(*this);
  const bool resumable = reason == ConversationEnd::CameraLost || reason == ConversationEnd::BusyLine ||
                         (reason == ConversationEnd::Handoff && !device);
  ConversationEnded ended;
  {
    std::scoped_lock lock(mutex_);
    carry_ = carry;
    if (reason == ConversationEnd::Handoff)
      snapshot_.handoff = device ? ConversationHandoff::Device : ConversationHandoff::Offering;
    snapshot_.state = resumable ? ConversationState::Resumable : ConversationState::Ended;
    snapshot_.endReason = reason;
    snapshot_.updatedAt = epochSeconds();
    snapshot_.resumableUntil =
        resumable ? snapshot_.updatedAt + (input_.config.resumable.count() / 1000) : 0;
    ended = {.snapshot = snapshot_, .reason = reason, .carry = device ? std::nullopt : carry};
  }
  LOG_INFO << "Voice watch: conversation " << input_.id << " ends (" << std::string(conversationEndToString(reason)) << ")";
  publish();
  if (input_.onEnded)
    input_.onEnded(ended);
}

void CameraConversation::addTurn(const std::string& role, const std::string& text)
{
  {
    std::scoped_lock lock(mutex_);
    snapshot_.turns.push_back({.id = nextTurnId_++, .role = role, .text = text, .at = epochSeconds(), .final = true});
    snapshot_.updatedAt = epochSeconds();
  }
  publish();
}

void CameraConversation::setState(ConversationState state)
{
  {
    std::scoped_lock lock(mutex_);
    if (snapshot_.state == state || closed_.load())
      return;
    snapshot_.state = state;
    snapshot_.updatedAt = epochSeconds();
  }
  publish();
}

void CameraConversation::publish()
{
  if (!input_.onUpdate)
    return;
  input_.onUpdate(snapshot());
}
