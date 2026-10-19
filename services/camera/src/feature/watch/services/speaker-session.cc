#include "speaker-session.hxx"

#include <feature/watch/services/speaker-program.hxx>

#include <camera/camera-errors.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace
{

namespace v1 = argus::camera::v1;

constexpr auto kReportEvery = std::chrono::seconds(1);

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

enum class SpeakerPhase : uint8_t
{
  Prologue,
  Body,
  Tail,
  Done
};

std::vector<int16_t> takePacket(std::deque<int16_t>& source, size_t samples)
{
  std::vector<int16_t> packet;
  packet.reserve(samples);
  const size_t taken = std::min(samples, source.size());
  packet.insert(packet.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(taken));
  source.erase(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(taken));
  packet.resize(samples, 0);
  return packet;
}

}

SpeakerSession::SpeakerSession(SpeakerSessionInput input)
    : input_(std::move(input)),
      uplink_({.sourceRate = input_.sourceRate,
               .packetMs = static_cast<int>(input_.config.packetMs),
               .maxQueuedMs = static_cast<int>(input_.config.speakQueueMs)}),
      lastInput_(Clock::now())
{
}

SpeakerSession::~SpeakerSession()
{
  stop();
  join();
}

void SpeakerSession::start()
{
  thread_ = std::thread([this] { run(); });
}

void SpeakerSession::push(std::span<const int16_t> samples)
{
  std::scoped_lock lock(mutex_);
  if (stopping_ || inputDone_)
    return;
  uplink_.push(samples);
  lastInput_ = Clock::now();
}

void SpeakerSession::finishInput()
{
  {
    std::scoped_lock lock(mutex_);
    inputDone_ = true;
  }
  wake_.notify_all();
}

void SpeakerSession::stop()
{
  {
    std::scoped_lock lock(mutex_);
    if (stopping_)
      return;
    stopping_ = true;
  }
  wake_.notify_all();
}

void SpeakerSession::join()
{
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
    thread_.join();
}

void SpeakerSession::emit(const SpeakerEventInput& event) const
{
  if (!input_.emit)
    return;
  v1::SpeakEvent out;
  out.set_state(event.state);
  out.set_played_ms(event.playedMs);
  out.set_refusal(event.refusal);
  out.set_detail(event.detail);
  {
    std::scoped_lock lock(mutex_);
    out.set_queued_ms(uplink_.queuedMs());
  }
  if (event.state == v1::SPEAK_STATE_DONE)
    out.set_playback_end_ms(nowMs() + input_.config.packetMs);
  input_.emit(out);
}

void SpeakerSession::run()
{
  if (!input_.driver) {
    emit({.state = v1::SPEAK_STATE_REFUSED,
          .playedMs = 0,
          .refusal = v1::WATCH_REFUSAL_NO_TALK,
          .detail = "no driver"});
    finished_.store(true);
    return;
  }
  auto opened = input_.driver->talkLine();
  if (!opened.line) {
    const bool busy = opened.error == CameraErrors::TalkLineBusy.message;
    emit({.state = busy ? v1::SPEAK_STATE_BUSY : v1::SPEAK_STATE_REFUSED,
          .playedMs = 0,
          .refusal = busy ? v1::WATCH_REFUSAL_NONE : v1::WATCH_REFUSAL_NO_TALK,
          .detail = opened.error});
    finished_.store(true);
    return;
  }
  const DriverResult ready = opened.line->open();
  if (!ready.ok) {
    LOG_WARN << "Camera watch: camera " << input_.cameraId << " refused the speaker line: " << ready.error;
    opened.line->close();
    emit({.state = v1::SPEAK_STATE_REFUSED,
          .playedMs = 0,
          .refusal = v1::WATCH_REFUSAL_NONE,
          .detail = ready.error});
    finished_.store(true);
    return;
  }
  serve(*opened.line);
  opened.line->close();
  finished_.store(true);
}

void SpeakerSession::serve(ICameraTalkLine& line)
{
  {
    std::scoped_lock lock(mutex_);
    if (stopping_)
      return;
    lastInput_ = Clock::now();
  }
  emit({.state = v1::SPEAK_STATE_READY, .playedMs = 0, .refusal = v1::WATCH_REFUSAL_NONE, .detail = {}});

  const CameraWatchConfig& config = input_.config;
  const auto packet = std::chrono::milliseconds(config.packetMs);
  const size_t packetSamples = uplink_.packetSamples();
  const std::vector<int16_t> prologueSamples = speakerPrologue({.rate = TalkUplink::kLineRate,
                                                                .earconMs = config.earconMs,
                                                                .leadSilenceMs = config.leadSilenceMs,
                                                                .earcon = input_.earcon});
  const std::vector<int16_t> tailSamples = silenceOf(TalkUplink::kLineRate, config.tailPadMs);
  std::deque<int16_t> prologue(prologueSamples.begin(), prologueSamples.end());
  std::deque<int16_t> tail(tailSamples.begin(), tailSamples.end());

  SpeakerPhase phase = prologue.empty() ? SpeakerPhase::Body : SpeakerPhase::Prologue;
  auto deadline = Clock::now();
  auto reportAt = deadline + kReportEvery;
  int64_t playedMs = 0;

  while (phase != SpeakerPhase::Done) {
    std::optional<std::vector<int16_t>> next;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_until(lock, deadline, [this] { return stopping_; });
      if (stopping_)
        return;
      if (phase == SpeakerPhase::Prologue) {
        next = takePacket(prologue, packetSamples);
        if (prologue.empty())
          phase = SpeakerPhase::Body;
      }
      else if (phase == SpeakerPhase::Body) {
        next = uplink_.next();
        const bool idle = Clock::now() - lastInput_ > std::chrono::milliseconds(config.speakIdleMs);
        if (!next && (inputDone_ || idle)) {
          next = uplink_.flush();
          inputDone_ = true;
          phase = tail.empty() ? SpeakerPhase::Done : SpeakerPhase::Tail;
        }
      }
      else if (phase == SpeakerPhase::Tail) {
        next = takePacket(tail, packetSamples);
        if (tail.empty())
          phase = SpeakerPhase::Done;
      }
    }

    const auto now = Clock::now();
    if (next) {
      if (const DriverResult written = line.write(*next); !written.ok) {
        LOG_WARN << "Camera watch: camera " << input_.cameraId << " speaker line lost: " << written.error;
        emit({.state = v1::SPEAK_STATE_LINE_LOST,
              .playedMs = playedMs,
              .refusal = v1::WATCH_REFUSAL_NONE,
              .detail = written.error});
        return;
      }
      playedMs += config.packetMs;
    }

    if (now >= reportAt) {
      reportAt = now + kReportEvery;
      if (input_.gate) {
        if (const v1::WatchRefusal refusal = input_.gate(); refusal != v1::WATCH_REFUSAL_NONE) {
          emit({.state = v1::SPEAK_STATE_REFUSED, .playedMs = playedMs, .refusal = refusal, .detail = {}});
          return;
        }
      }
      emit({.state = v1::SPEAK_STATE_PLAYING, .playedMs = playedMs, .refusal = v1::WATCH_REFUSAL_NONE, .detail = {}});
    }

    deadline += packet;
    if (deadline + packet < now)
      deadline = now;
  }
  emit({.state = v1::SPEAK_STATE_DONE, .playedMs = playedMs, .refusal = v1::WATCH_REFUSAL_NONE, .detail = {}});
}
