#include "voice-rpc-service.hxx"

#include <runtime/blocking-pool.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <drogon/drogon.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{

class VoiceSessionStream final
    : public grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                                     argus::voice::v1::ServerFrame>,
      public VoiceSessionSink,
      public std::enable_shared_from_this<VoiceSessionStream>
{
public:
  struct Input
  {
    VoiceSessionService& sessions;
    grpc::CallbackServerContext* context;
    const std::vector<argus::client::CallerCredential>& callers;
    std::shared_ptr<std::atomic<int>> live;
    std::uint64_t id{0};
    std::function<void(std::uint64_t)> retire;
    VoiceCleanupDispatch dispatchCleanup;
  };

  explicit VoiceSessionStream(const Input& input)
      : sessions_(input.sessions), context_(input.context), callers_(input.callers),
        live_(input.live), id_(input.id), retire_(input.retire),
        dispatchCleanup_(input.dispatchCleanup)
  {
    live_->fetch_add(1);
  }

  ~VoiceSessionStream() override
  {
    if (!cleaningUp_.exchange(true))
      live_->fetch_sub(1);
  }

  void begin()
  {
    if (!authorized()) {
      {
        std::scoped_lock lock(writeMutex_);
        finishing_ = true;
      }
      Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                          "argus-sync caller credential required"));
      return;
    }
    StartRead(&read_);
  }

  void OnReadDone(bool ok) override
  {
    if (!ok) {
      closer_ = std::jthread([this] { endSession(); });
      return;
    }
    handleFrame(read_);
    read_ = {};
    StartRead(&read_);
  }

  void OnWriteDone(bool ok) override
  {
    std::scoped_lock lock(writeMutex_);
    writing_ = false;
    if (!ok) {
      finished_.store(true);
      queue_.clear();
    }
    else if (!queue_.empty())
      queue_.pop_front();
    pumpLocked();
    drainCv_.notify_all();
  }

  void OnCancel() override { cancelled_.store(true); }

  void OnDone() override
  {
    done_.store(true);
    {
      std::scoped_lock lock(writeMutex_);
      finished_.store(true);
      queue_.clear();
    }
    const auto self = shared_from_this();
    const std::function<void()> cleanup = [self] { self->finish(); };
    try {
      dispatchCleanup_(cleanup);
    }
    catch (const std::exception& e) {
      LOG_WARN << "Voice: stream cleanup could not be queued (" << e.what()
               << "), running it on the completion thread";
      finish();
    }
    catch (...) {
      LOG_WARN << "Voice: stream cleanup could not be queued, running it on the completion thread";
      finish();
    }
  }

  bool connected() const override
  {
    return !finished_.load() && !cancelled_.load() && !done_.load();
  }

  void sendServerFrame(argus::voice::v1::ServerFrame frame) override
  {
    std::scoped_lock lock(writeMutex_);
    if (finished_.load())
      return;
    if (queue_.size() >= kMaxPendingWrites) {
      LOG_WARN << "Voice: stream write backlog, dropping frame";
      return;
    }
    queue_.push_back(std::move(frame));
    pumpLocked();
  }

  void finish()
  {
    if (cleaningUp_.exchange(true))
      return;
    try {
      if (closer_.joinable())
        closer_.join();
      sessions_.stop(*this);
    }
    catch (const std::exception& e) {
      LOG_WARN << "Voice: stream teardown failed: " << e.what();
    }
    catch (...) {
      LOG_WARN << "Voice: stream teardown failed";
    }
    live_->fetch_sub(1);
    retire_(id_);
  }

private:
  bool authorized() const
  {
    if (!argus::client::authorizeCaller(context_, callers_).has_value())
      return false;
    bool user = false;
    bool role = false;
    for (const auto& [key, value] : context_->client_metadata()) {
      if (key == "x-argus-user")
        user = true;
      else if (key == "x-argus-role")
        role = true;
    }
    return user && role;
  }

  void handleFrame(const argus::voice::v1::ClientFrame& frame)
  {
    switch (frame.body_case()) {
      case argus::voice::v1::ClientFrame::kStart:
        sessions_.start(*this, frame.start());
        break;
      case argus::voice::v1::ClientFrame::kStop:
        sessions_.stop(*this);
        break;
      case argus::voice::v1::ClientFrame::kSkip:
        sessions_.skip(*this);
        break;
      case argus::voice::v1::ClientFrame::kContext:
        sessions_.context(*this, frame.context());
        break;
      case argus::voice::v1::ClientFrame::kActionResult:
        sessions_.actionResult(*this, frame.action_result());
        break;
      case argus::voice::v1::ClientFrame::kMute:
        sessions_.mute(*this, frame.mute().muted());
        break;
      case argus::voice::v1::ClientFrame::kFarewell:
        sessions_.farewell(*this, farewellReasonOf(frame.farewell().reason()));
        break;
      case argus::voice::v1::ClientFrame::kPcm:
        sessions_.feedPcm(*this, {.data = frame.pcm().data(),
                                  .size = static_cast<size_t>(
                                      frame.pcm().size())});
        break;
      default:
        LOG_WARN << "Voice: client frame without body";
        break;
    }
  }

  void endSession()
  {
    sessions_.stop(*this);
    drain();
    std::scoped_lock lock(writeMutex_);
    if (finishing_ || done_.load())
      return;
    finishing_ = true;
    Finish(grpc::Status::OK);
  }

  void drain()
  {
    std::unique_lock<std::mutex> lock(writeMutex_);
    drainCv_.wait_for(lock, std::chrono::seconds(2), [this] {
      return queue_.empty() || finished_.load();
    });
  }

  void pumpLocked()
  {
    if (writing_ || queue_.empty() || finished_.load())
      return;
    writing_ = true;
    StartWrite(&queue_.front());
  }

  static constexpr size_t kMaxPendingWrites = 512;

  VoiceSessionService& sessions_;
  grpc::CallbackServerContext* context_;
  const std::vector<argus::client::CallerCredential>& callers_;
  std::mutex writeMutex_;
  std::condition_variable drainCv_;
  std::deque<argus::voice::v1::ServerFrame> queue_;
  bool writing_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> done_{false};
  std::atomic<bool> cleaningUp_{false};
  bool finishing_{false};
  argus::voice::v1::ClientFrame read_;
  std::shared_ptr<std::atomic<int>> live_;
  std::uint64_t id_{0};
  std::function<void(std::uint64_t)> retire_;
  VoiceCleanupDispatch dispatchCleanup_;
  std::jthread closer_;
};

}

struct VoiceRpcService::Streams
{
  ~Streams()
  {
    std::unordered_map<std::uint64_t, std::shared_ptr<VoiceSessionStream>> doomed;
    {
      std::scoped_lock lock(mutex);
      doomed.swap(live);
    }
    for (const auto& [id, stream] : doomed)
      stream->finish();
  }

  void add(std::uint64_t id, std::shared_ptr<VoiceSessionStream> stream)
  {
    std::scoped_lock lock(mutex);
    live.emplace(id, std::move(stream));
  }

  void retire(std::uint64_t id)
  {
    std::scoped_lock lock(mutex);
    live.erase(id);
  }

  std::mutex mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<VoiceSessionStream>> live;
};

namespace
{

VoiceCleanupDispatch cleanupDispatchOf(const VoiceCleanupDispatch& dispatch)
{
  if (dispatch)
    return dispatch;
  return [](std::function<void()> job) {
    blocking_pool::submit(BlockingLane::Light, std::move(job));
  };
}

}

VoiceRpcService::VoiceRpcService(VoiceRpcInput input)
    : sessions_(*input.sessions),
      syncCallers_({argus::client::CallerCredential{
          .service = "argus-sync", .secret = std::move(input.syncCallerSecret)}}),
      notificationCallers_({argus::client::CallerCredential{
          .service = "argus-notification", .secret = std::move(input.notificationCallerSecret)}}),
      rooms_(input.rooms),
      streams_(std::make_shared<Streams>()),
      dispatchCleanup_(cleanupDispatchOf(input.dispatchCleanup))
{
}

grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                        argus::voice::v1::ServerFrame>*
VoiceRpcService::Connect(grpc::CallbackServerContext* context)
{
  const std::uint64_t id = nextStreamId_.fetch_add(1);
  auto stream = std::make_shared<VoiceSessionStream>(VoiceSessionStream::Input{
      .sessions = sessions_,
      .context = context,
      .callers = syncCallers_,
      .live = live_,
      .id = id,
      .retire = [registry = std::weak_ptr<Streams>(streams_)](std::uint64_t streamId) {
        if (const auto streams = registry.lock())
          streams->retire(streamId);
      },
      .dispatchCleanup = dispatchCleanup_});
  streams_->add(id, stream);
  try {
    stream->begin();
  }
  catch (...) {
    streams_->retire(id);
    throw;
  }
  return stream.get();
}

grpc::ServerUnaryReactor* VoiceRpcService::JoinRoom(grpc::CallbackServerContext* context,
                                                    const argus::voice::v1::RtcJoin* request,
                                                    argus::voice::v1::RtcJoined* reply)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, syncCallers_).has_value()) {
    reactor->Finish({grpc::StatusCode::UNAUTHENTICATED, "argus-sync caller credential required"});
    return reactor;
  }
  if (rooms_ == nullptr) {
    reactor->Finish({grpc::StatusCode::UNAVAILABLE, "realtime calls are not configured"});
    return reactor;
  }
  rooms_->joinRoom(*request, [reactor, reply](const grpc::Status& status, argus::voice::v1::RtcJoined joined) {
    *reply = std::move(joined);
    reactor->Finish(status);
  });
  return reactor;
}

grpc::ServerUnaryReactor* VoiceRpcService::Farewell(grpc::CallbackServerContext* context,
                                                    const argus::voice::v1::RtcFarewell* request,
                                                    argus::voice::v1::RtcFarewellDone* reply)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, syncCallers_).has_value()) {
    reactor->Finish({grpc::StatusCode::UNAUTHENTICATED, "argus-sync caller credential required"});
    return reactor;
  }
  if (rooms_ == nullptr) {
    reactor->Finish({grpc::StatusCode::UNAVAILABLE, "realtime calls are not configured"});
    return reactor;
  }
  rooms_->farewellRoom(*request, [reactor, reply](bool played) {
    reply->set_played(played);
    reactor->Finish(grpc::Status::OK);
  });
  return reactor;
}

grpc::ServerUnaryReactor* VoiceRpcService::Announce(grpc::CallbackServerContext* context,
                                                    const argus::voice::v1::AnnounceRequest* request,
                                                    argus::voice::v1::AnnounceResponse* reply)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, notificationCallers_).has_value()) {
    reactor->Finish({grpc::StatusCode::UNAUTHENTICATED, "argus-notification caller credential required"});
    return reactor;
  }
  if (request->user_id() <= 0 || request->text().empty()) {
    reactor->Finish({grpc::StatusCode::INVALID_ARGUMENT, "user_id and text are required"});
    return reactor;
  }
  reply->set_delivered(sessions_.announce(request->user_id(), request->text()));
  LOG_INFO << "Voice: announce kind=" << request->kind() << " call=" << request->call_id()
           << " delivered=" << reply->delivered();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}
