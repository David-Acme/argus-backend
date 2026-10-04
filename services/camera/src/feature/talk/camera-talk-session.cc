#include "camera-talk-session.hxx"

#include <camera/camera-errors.hxx>
#include <trantor/utils/Logger.h>

#include <string_view>
#include <utility>

namespace
{
constexpr auto kStatsEvery = std::chrono::seconds(1);

Json::Value frame(std::string_view type, Json::Value payload)
{
  Json::Value out(Json::objectValue);
  out["type"] = std::string(type);
  out["payload"] = std::move(payload);
  return out;
}
}

CameraTalkSession::CameraTalkSession(TalkSessionConfig config,
                                     std::shared_ptr<ICameraDriver> driver,
                                     TalkSessionEvents events)
    : config_(config),
      driver_(std::move(driver)),
      events_(std::move(events)),
      uplink_({.sourceRate = config.sampleRate,
               .packetMs = config.packetMs,
               .maxQueuedMs = config.maxQueuedMs}),
      lastAudio_(Clock::now())
{
}

CameraTalkSession::~CameraTalkSession()
{
  stop("stopped");
  join();
}

void CameraTalkSession::start()
{
  thread_ = std::thread([this] { run(); });
}

void CameraTalkSession::push(std::span<const int16_t> samples)
{
  std::scoped_lock lock(mutex_);
  if (stopping_)
    return;
  uplink_.push(samples);
  lastAudio_ = Clock::now();
}

void CameraTalkSession::stop(const std::string& reason)
{
  {
    std::scoped_lock lock(mutex_);
    if (stopping_)
      return;
    stopping_ = true;
    reason_ = reason;
  }
  wake_.notify_all();
}

void CameraTalkSession::join()
{
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
    thread_.join();
}

void CameraTalkSession::refuse(int status, const std::string& error) const
{
  Json::Value out(Json::objectValue);
  out["type"] = "camera:talk:start_error";
  out["status"] = status;
  out["error"] = error;
  out["cameraId"] = static_cast<Json::Int64>(config_.cameraId);
  events_.send(out);
}

void CameraTalkSession::closed(const std::string& reason) const
{
  Json::Value payload(Json::objectValue);
  payload["cameraId"] = static_cast<Json::Int64>(config_.cameraId);
  payload["reason"] = reason;
  events_.send(frame("camera:talk:closed", payload));
}

void CameraTalkSession::run()
{
  auto opened = driver_->talkLine();
  if (!opened.line) {
    const bool busy = opened.error == CameraErrors::TalkLineBusy.message;
    refuse(busy ? CameraErrors::TalkLineBusy.status : CameraErrors::TalkUnavailable.status,
           opened.error);
    finished_.store(true);
    return;
  }
  const auto ready = opened.line->open();
  if (!ready.ok) {
    LOG_WARN << "camera talk: camera " << config_.cameraId << " refused the line: " << ready.error;
    opened.line->close();
    refuse(CameraErrors::CameraUnreachable.status, ready.error);
    finished_.store(true);
    return;
  }
  serve(*opened.line);
  opened.line->close();
  std::string reason;
  {
    std::scoped_lock lock(mutex_);
    reason = reason_.empty() ? std::string("stopped") : reason_;
  }
  closed(reason);
  finished_.store(true);
}

void CameraTalkSession::serve(ICameraTalkLine& line)
{
  {
    std::scoped_lock lock(mutex_);
    lastAudio_ = Clock::now();
    if (stopping_)
      return;
  }
  Json::Value ready(Json::objectValue);
  ready["cameraId"] = static_cast<Json::Int64>(config_.cameraId);
  ready["sampleRate"] = config_.sampleRate;
  ready["packetMs"] = config_.packetMs;
  events_.send(frame("camera:talk:ready", ready));

  const auto packet = std::chrono::milliseconds(config_.packetMs);
  const auto started = Clock::now();
  auto deadline = started;
  auto statsAt = started + kStatsEvery;
  int64_t sentMs = 0;
  int64_t underruns = 0;
  bool speaking = false;

  for (;;) {
    std::optional<std::vector<int16_t>> next;
    int queuedMs = 0;
    int64_t droppedMs = 0;
    Clock::time_point lastAudio;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_until(lock, deadline, [this] { return stopping_; });
      if (stopping_)
        return;
      next = uplink_.next();
      queuedMs = uplink_.queuedMs();
      droppedMs = uplink_.droppedMs();
      lastAudio = lastAudio_;
    }

    const auto now = Clock::now();
    if (next) {
      const auto written = line.write(*next);
      if (!written.ok) {
        LOG_WARN << "camera talk: camera " << config_.cameraId << " line lost: " << written.error;
        stop("line_lost");
        return;
      }
      sentMs += config_.packetMs;
      speaking = true;
    }
    else if (speaking) {
      ++underruns;
      speaking = false;
    }

    if (now - lastAudio > std::chrono::milliseconds(config_.idleMs)) {
      stop("idle");
      return;
    }
    if (now - started > std::chrono::milliseconds(config_.maxMs)) {
      stop("limit");
      return;
    }
    if (now >= statsAt) {
      Json::Value stats(Json::objectValue);
      stats["cameraId"] = static_cast<Json::Int64>(config_.cameraId);
      stats["queuedMs"] = queuedMs;
      stats["sentMs"] = static_cast<Json::Int64>(sentMs);
      stats["droppedMs"] = static_cast<Json::Int64>(droppedMs);
      stats["underruns"] = static_cast<Json::Int64>(underruns);
      events_.send(frame("camera:talk:state", stats));
      statsAt = now + kStatsEvery;
    }

    deadline += packet;
    if (deadline + packet < now)
      deadline = now;
  }
}
