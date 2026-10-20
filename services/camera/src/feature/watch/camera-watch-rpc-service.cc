#include "camera-watch-rpc-service.hxx"

#include <feature/watch/services/speaker-program.hxx>
#include <feature/watch/services/speaker-session.hxx>
#include <feature/watch/services/watch-gate.hxx>

#include <runtime/blocking-pool.hxx>
#include <shared/services/stream/person-sightings.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace
{

namespace v1 = argus::camera::v1;

std::string pcmBytes(std::span<const int16_t> pcm)
{
  std::string bytes;
  bytes.reserve(pcm.size() * 2);
  for (const int16_t sample : pcm) {
    const auto value = static_cast<uint16_t>(sample);
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  }
  return bytes;
}

grpc::Status refusalStatus(v1::WatchRefusal refusal)
{
  return {grpc::StatusCode::FAILED_PRECONDITION, std::string(watchRefusalName(refusal))};
}

struct MicrophoneStreamInput
{
  std::shared_ptr<MicrophoneHub> hub;
  std::shared_ptr<WatchReactorRegistry> registry;
  uint64_t id{0};
  size_t queueFrames{64};
};

struct MicrophoneOpen
{
  bool authorized{false};
  int64_t cameraId{0};
};

class MicrophoneStream final : public grpc::ServerWriteReactor<v1::MicrophoneFrame>
{
public:
  explicit MicrophoneStream(MicrophoneStreamInput input) : input_(std::move(input)) {}

  void begin(const MicrophoneOpen& open)
  {
    if (!open.authorized) {
      requestFinish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "argus-voice caller credential required"));
      return;
    }
    if (open.cameraId <= 0) {
      requestFinish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_id is required"));
      return;
    }
    const MicrophoneSubscribed subscribed = input_.hub->subscribe(
        {.cameraId = open.cameraId,
         .onFrame = [this](const MicrophoneChunk& chunk) { push(chunk); },
         .onClosed = [this](v1::WatchRefusal refusal) { requestFinish(refusalStatus(refusal)); }});
    if (subscribed.id == 0) {
      requestFinish(refusalStatus(subscribed.refusal));
      return;
    }
    subscription_.store(subscribed.id);
  }

  void OnWriteDone(bool ok) override
  {
    std::scoped_lock lock(mutex_);
    writing_ = false;
    if (!ok) {
      queue_.clear();
      finishing_ = true;
      if (!pendingFinish_)
        pendingFinish_ = grpc::Status::CANCELLED;
    }
    else if (!queue_.empty()) {
      queue_.pop_front();
    }
    pumpLocked();
  }

  void OnCancel() override { requestFinish(grpc::Status::CANCELLED); }

  void OnDone() override
  {
    if (const uint64_t subscription = subscription_.exchange(0); subscription != 0)
      input_.hub->unsubscribe(subscription);
    const std::shared_ptr<void> self = input_.registry->take(input_.id);
  }

private:
  void push(const MicrophoneChunk& chunk)
  {
    std::scoped_lock lock(mutex_);
    if (finishing_)
      return;
    if (queue_.size() >= input_.queueFrames) {
      if (writing_ && queue_.size() > 1)
        queue_.erase(queue_.begin() + 1);
      else if (!writing_)
        queue_.pop_front();
      droppedSinceSent_ = true;
    }
    v1::MicrophoneFrame frame;
    frame.set_pcm(pcmBytes(chunk.pcm));
    frame.set_sequence(chunk.sequence);
    frame.set_captured_at_ms(chunk.capturedAtMs);
    const bool dropped = std::exchange(droppedSinceSent_, false);
    frame.set_gap(chunk.gap || dropped);
    queue_.push_back(std::move(frame));
    pumpLocked();
  }

  void requestFinish(const grpc::Status& status)
  {
    std::scoped_lock lock(mutex_);
    if (finishing_)
      return;
    finishing_ = true;
    pendingFinish_ = status;
    if (writing_) {
      while (queue_.size() > 1)
        queue_.pop_back();
    }
    else {
      queue_.clear();
    }
    pumpLocked();
  }

  void pumpLocked()
  {
    if (writing_ || finished_)
      return;
    if (!queue_.empty() && !finishing_) {
      writing_ = true;
      StartWrite(&queue_.front());
      return;
    }
    if (finishing_ && pendingFinish_) {
      finished_ = true;
      Finish(*pendingFinish_);
      pendingFinish_.reset();
    }
  }

  MicrophoneStreamInput input_;
  std::atomic<uint64_t> subscription_{0};
  std::mutex mutex_;
  std::deque<v1::MicrophoneFrame> queue_;
  bool writing_{false};
  bool finishing_{false};
  bool finished_{false};
  bool droppedSinceSent_{false};
  std::optional<grpc::Status> pendingFinish_;
};

struct SpeakStreamInput
{
  std::shared_ptr<WatchReactorRegistry> registry;
  std::shared_ptr<WatchCameraBoard> board;
  std::function<v1::WatchRefusal(int64_t)> gate;
  CameraWatchConfig config;
  uint64_t id{0};
};

class SpeakStream final : public grpc::ServerBidiReactor<v1::SpeakFrame, v1::SpeakEvent>
{
public:
  explicit SpeakStream(SpeakStreamInput input) : input_(std::move(input)) {}

  void begin(bool authorized)
  {
    if (!authorized) {
      requestFinish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "argus-voice caller credential required"));
      return;
    }
    StartRead(&read_);
  }

  void OnReadDone(bool ok) override
  {
    if (!ok) {
      const std::shared_ptr<SpeakerSession> session = currentSession();
      if (session)
        session->finishInput();
      else
        requestFinish(grpc::Status::OK);
      return;
    }
    if (handle(read_)) {
      read_.Clear();
      StartRead(&read_);
    }
  }

  void OnWriteDone(bool ok) override
  {
    std::scoped_lock lock(mutex_);
    writing_ = false;
    if (!ok) {
      queue_.clear();
      finishing_ = true;
      if (!pendingFinish_)
        pendingFinish_ = grpc::Status::CANCELLED;
    }
    else if (!queue_.empty()) {
      queue_.pop_front();
    }
    pumpLocked();
  }

  void OnCancel() override
  {
    if (const std::shared_ptr<SpeakerSession> session = currentSession())
      session->stop();
    requestFinish(grpc::Status::CANCELLED);
  }

  void OnDone() override
  {
    std::shared_ptr<SpeakerSession> session;
    {
      std::scoped_lock lock(mutex_);
      session = std::move(session_);
    }
    if (session) {
      session->stop();
      session->join();
    }
    const std::shared_ptr<void> self = input_.registry->take(input_.id);
  }

private:
  [[nodiscard]] std::shared_ptr<SpeakerSession> currentSession()
  {
    std::scoped_lock lock(mutex_);
    return session_;
  }

  bool handle(const v1::SpeakFrame& frame)
  {
    switch (frame.body_case()) {
      case v1::SpeakFrame::kOpen:
        return open(frame.open());
      case v1::SpeakFrame::kPcm: {
        const std::shared_ptr<SpeakerSession> session = currentSession();
        if (!session) {
          refuse(v1::WATCH_REFUSAL_NONE, "pcm before open");
          return false;
        }
        const std::vector<int16_t> samples = pcmSamplesOf(frame.pcm());
        session->push(samples);
        return true;
      }
      case v1::SpeakFrame::kEnd: {
        if (const std::shared_ptr<SpeakerSession> session = currentSession())
          session->finishInput();
        else
          requestFinish(grpc::Status::OK);
        return true;
      }
      case v1::SpeakFrame::BODY_NOT_SET:
        break;
    }
    return true;
  }

  bool open(const v1::SpeakOpen& request)
  {
    if (currentSession())
      return true;
    const int64_t cameraId = request.camera_id();
    const std::optional<WatchCameraFlags> flags = input_.board->flags(cameraId);
    v1::WatchRefusal refusal = input_.gate ? input_.gate(cameraId) : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
    if (!flags)
      refusal = v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
    if (refusal == v1::WATCH_REFUSAL_NONE && !flags->talk)
      refusal = v1::WATCH_REFUSAL_NO_TALK;
    const std::optional<CameraSchema> schema = input_.board->schema(cameraId);
    std::shared_ptr<ICameraDriver> driver =
        schema ? CameraDriverRegistry::instance().driverFor(*schema) : nullptr;
    if (refusal == v1::WATCH_REFUSAL_NONE && !driver)
      refusal = v1::WATCH_REFUSAL_NO_TALK;
    const int rate = request.sample_rate();
    if (refusal == v1::WATCH_REFUSAL_NONE && (rate < 8000 || rate > 48000)) {
      refuse(v1::WATCH_REFUSAL_NONE, "unsupported sample rate");
      return false;
    }
    if (refusal != v1::WATCH_REFUSAL_NONE) {
      refuse(refusal, {});
      return false;
    }
    auto session = std::make_shared<SpeakerSession>(SpeakerSessionInput{
        .cameraId = cameraId,
        .sourceRate = rate,
        .earcon = request.earcon(),
        .config = input_.config,
        .driver = std::move(driver),
        .gate = [gate = input_.gate, cameraId] { return gate ? gate(cameraId) : v1::WATCH_REFUSAL_NONE; },
        .emit = [this](const v1::SpeakEvent& event) { relay(event); }});
    {
      std::scoped_lock lock(mutex_);
      if (finishing_)
        return false;
      session_ = session;
    }
    LOG_INFO << "Camera watch: Argus speaks on camera " << cameraId << " for conversation "
             << request.conversation_id();
    session->start();
    return true;
  }

  void relay(const v1::SpeakEvent& event)
  {
    const v1::SpeakState state = event.state();
    send(event);
    if (state == v1::SPEAK_STATE_DONE || state == v1::SPEAK_STATE_BUSY || state == v1::SPEAK_STATE_REFUSED ||
        state == v1::SPEAK_STATE_LINE_LOST)
      requestFinish(grpc::Status::OK);
  }

  void refuse(v1::WatchRefusal refusal, const std::string& detail)
  {
    v1::SpeakEvent event;
    event.set_state(v1::SPEAK_STATE_REFUSED);
    event.set_refusal(refusal);
    event.set_detail(detail);
    send(event);
    requestFinish(grpc::Status::OK);
  }

  void send(const v1::SpeakEvent& event)
  {
    std::scoped_lock lock(mutex_);
    if (finishing_)
      return;
    queue_.push_back(event);
    pumpLocked();
  }

  void requestFinish(const grpc::Status& status)
  {
    std::scoped_lock lock(mutex_);
    if (finishing_)
      return;
    finishing_ = true;
    pendingFinish_ = status;
    pumpLocked();
  }

  void pumpLocked()
  {
    if (writing_ || finished_)
      return;
    if (!queue_.empty()) {
      writing_ = true;
      StartWrite(&queue_.front());
      return;
    }
    if (finishing_ && pendingFinish_) {
      finished_ = true;
      Finish(*pendingFinish_);
      pendingFinish_.reset();
    }
  }

  SpeakStreamInput input_;
  v1::SpeakFrame read_;
  std::mutex mutex_;
  std::shared_ptr<SpeakerSession> session_;
  std::deque<v1::SpeakEvent> queue_;
  bool writing_{false};
  bool finishing_{false};
  bool finished_{false};
  std::optional<grpc::Status> pendingFinish_;
};

}

void WatchReactorRegistry::add(uint64_t id, std::shared_ptr<void> reactor)
{
  std::scoped_lock lock(mutex_);
  live_.emplace(id, std::move(reactor));
}

std::shared_ptr<void> WatchReactorRegistry::take(uint64_t id)
{
  std::scoped_lock lock(mutex_);
  const auto found = live_.find(id);
  if (found == live_.end())
    return nullptr;
  std::shared_ptr<void> reactor = std::move(found->second);
  live_.erase(found);
  return reactor;
}

size_t WatchReactorRegistry::size() const
{
  std::scoped_lock lock(mutex_);
  return live_.size();
}

CameraWatchRpcService::CameraWatchRpcService(CameraWatchServiceInput input)
    : callers_(std::move(input.callers)),
      hub_(std::move(input.hub)),
      board_(std::move(input.board)),
      gate_(std::move(input.gate)),
      config_(input.config),
      identify_(std::move(input.identify)),
      aimer_(std::move(input.aimer))
{
}

bool CameraWatchRpcService::authorized(const grpc::CallbackServerContext* context) const
{
  const std::optional<std::string> caller = argus::client::authorizeCaller(context, callers_);
  return caller && *caller == kWatchVoiceCaller;
}

bool CameraWatchRpcService::readsSightings(const grpc::CallbackServerContext* context) const
{
  return argus::client::authorizeCaller(context, callers_).has_value();
}

grpc::ServerUnaryReactor* CameraWatchRpcService::ListWatch(grpc::CallbackServerContext* context,
                                                           const v1::ListWatchRequest*,
                                                           v1::ListWatchResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!readsSightings(context)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "argus-voice caller credential required"));
    return reactor;
  }
  for (const WatchCameraFlags& flags : board_->all()) {
    const v1::WatchRefusal refusal = gate_ ? gate_(flags.id) : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
    auto* camera = response->add_cameras();
    camera->set_camera_id(flags.id);
    camera->set_name(flags.name);
    camera->set_listening(refusal == v1::WATCH_REFUSAL_NONE);
    camera->set_refusal(refusal);
    camera->set_talk(flags.talk);
    camera->set_pan_tilt(flags.panTilt);
    camera->set_tv_exclusion(flags.tvExclusion);
  }
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerWriteReactor<v1::MicrophoneFrame>*
CameraWatchRpcService::OpenMicrophone(grpc::CallbackServerContext* context, const v1::MicrophoneRequest* request)
{
  const uint64_t id = nextId_.fetch_add(1);
  auto stream = std::make_shared<MicrophoneStream>(MicrophoneStreamInput{
      .hub = hub_,
      .registry = reactors_,
      .id = id,
      .queueFrames = static_cast<size_t>(config_.micQueueFrames)});
  reactors_->add(id, stream);
  stream->begin({.authorized = authorized(context) && !stopping_.load(), .cameraId = request->camera_id()});
  return stream.get();
}

grpc::ServerBidiReactor<v1::SpeakFrame, v1::SpeakEvent>* CameraWatchRpcService::Speak(grpc::CallbackServerContext* context)
{
  const uint64_t id = nextId_.fetch_add(1);
  auto stream = std::make_shared<SpeakStream>(SpeakStreamInput{
      .registry = reactors_, .board = board_, .gate = gate_, .config = config_, .id = id});
  reactors_->add(id, stream);
  stream->begin(authorized(context) && !stopping_.load());
  return stream.get();
}

grpc::ServerUnaryReactor* CameraWatchRpcService::FaceWindow(grpc::CallbackServerContext* context,
                                                            const v1::FaceWindowRequest* request,
                                                            v1::FaceWindowResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!authorized(context)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "argus-voice caller credential required"));
    return reactor;
  }
  const int64_t cameraId = request->camera_id();
  if (const v1::WatchRefusal refusal = gate_ ? gate_(cameraId) : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
      refusal != v1::WATCH_REFUSAL_NONE) {
    reactor->Finish(refusalStatus(refusal));
    return reactor;
  }
  if (!identify_) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "no identity target to match faces with"));
    return reactor;
  }
  const auto window = std::chrono::milliseconds(
      std::clamp<int64_t>(request->window_ms(), 100, config_.faceWindowMaxMs));
  const auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  blocking_pool::submit(BlockingLane::Light, [this, reactor, response, cameraId, window, startMs] {
    std::this_thread::sleep_for(window);
    if (const v1::WatchRefusal refusal = gate_(cameraId); refusal != v1::WATCH_REFUSAL_NONE) {
      reactor->Finish(refusalStatus(refusal));
      return;
    }
    const int64_t since = startMs - window.count();
    std::vector<CameraSnapshot> crops = SnapshotStore::instance().personCropsSince({.cameraId = cameraId, .sinceMs = since});
    if (crops.empty()) {
      if (const auto frame = SnapshotStore::instance().frame(cameraId); frame && frame->atMs >= since)
        crops.push_back(*frame);
    }
    std::ranges::sort(crops, std::ranges::greater{}, &CameraSnapshot::atMs);
    if (std::cmp_greater(crops.size(), config_.faceWindowMaxCrops))
      crops.resize(static_cast<size_t>(config_.faceWindowMaxCrops));
    std::vector<int64_t> seen;
    for (const CameraSnapshot& crop : crops) {
      const auto found = identify_({.image = crop.jpeg, .cameraId = cameraId, .observedAt = crop.atMs / 1000});
      if (!found || (!found->face_found() && !found->matched()))
        continue;
      if (found->person_id() > 0 && std::ranges::find(seen, found->person_id()) != seen.end())
        continue;
      seen.push_back(found->person_id());
      auto* face = response->add_faces();
      face->set_person_id(found->person_id());
      face->set_user_id(found->user_id());
      face->set_name(found->name());
      face->set_role(found->role());
      face->set_confidence(found->confidence());
      face->set_quality(found->face_quality());
      face->set_visitor(found->visitor());
      face->set_trusted(found->trusted());
      face->set_matched(found->matched());
    }
    response->set_crops(static_cast<int32_t>(crops.size()));
    reactor->Finish(grpc::Status::OK);
  });
  return reactor;
}

grpc::ServerUnaryReactor* CameraWatchRpcService::Sightings(grpc::CallbackServerContext* context,
                                                           const v1::SightingsRequest* request,
                                                           v1::SightingsResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!readsSightings(context)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "camera watch caller credential required"));
    return reactor;
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const int64_t since = std::max(request->since_ms(), now - config_.sightingHoldMs);
  for (const PersonSighting& seen : PersonSightings::instance().since(since)) {
    auto* sighting = response->add_sightings();
    sighting->set_camera_id(seen.cameraId);
    sighting->set_track_id(seen.trackId);
    sighting->set_person_id(seen.personId);
    sighting->set_known(seen.known);
    sighting->set_center_x(seen.centerX);
    sighting->set_center_y(seen.centerY);
    sighting->set_width(seen.width);
    sighting->set_height(seen.height);
    sighting->set_night(seen.night);
    sighting->set_at_ms(seen.atMs);
  }
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* CameraWatchRpcService::Aim(grpc::CallbackServerContext* context,
                                                     const v1::AimRequest* request,
                                                     v1::AimResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!authorized(context)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "argus-voice caller credential required"));
    return reactor;
  }
  if (request->camera_id() <= 0 || request->conversation_id().empty()) {
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "camera_id and conversation_id are required"));
    return reactor;
  }
  if (!aimer_ || stopping_.load()) {
    response->set_outcome(v1::AIM_OUTCOME_NO_PTZ);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  const AimTarget target{.cameraId = request->camera_id(), .conversationId = request->conversation_id()};
  const v1::AimMode mode = request->mode();
  blocking_pool::submit(BlockingLane::Light, [aimer = aimer_, reactor, response, target, mode] {
    *response = mode == v1::AIM_MODE_RELEASE ? aimer->release(target) : aimer->find(target);
    reactor->Finish(grpc::Status::OK);
  });
  return reactor;
}

void CameraWatchRpcService::requestStop()
{
  stopping_.store(true);
  if (hub_)
    hub_->shutdown();
}

bool CameraWatchRpcService::drained() const
{
  return stopping_.load() && (!hub_ || hub_->readers() == 0);
}
