#include "voice-rpc-service.hxx"

#include <runtime/blocking-pool.hxx>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <drogon/drogon.h>
#include <thread>

namespace
{

class VoiceSessionStream final
    : public grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                                     argus::voice::v1::ServerFrame>,
      public VoiceSessionSink
{
public:
  struct Input
  {
    VoiceSessionService& sessions;
    grpc::CallbackServerContext* context;
    const std::vector<argus::client::CallerCredential>& callers;
    std::shared_ptr<std::atomic<int>> live;
  };

  explicit VoiceSessionStream(const Input& input)
      : sessions_(input.sessions), context_(input.context), callers_(input.callers),
        live_(input.live)
  {
    live_->fetch_add(1);
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

  void OnDone() override
  {
    blocking_pool::submit(BlockingLane::Light, [this] {
      if (closer_.joinable())
        closer_.join();
      sessions_.stop(*this);
      const auto live = live_;
      delete this;
      live->fetch_sub(1);
    });
  }

  bool connected() const override
  {
    return !finished_.load() && !context_->IsCancelled();
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
    if (finishing_ || context_->IsCancelled())
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
  bool finishing_{false};
  argus::voice::v1::ClientFrame read_;
  std::shared_ptr<std::atomic<int>> live_;
  std::jthread closer_;
};

}

VoiceRpcService::VoiceRpcService(VoiceRpcInput input)
    : sessions_(*input.sessions),
      syncCallers_({argus::client::CallerCredential{
          .service = "argus-sync", .secret = std::move(input.syncCallerSecret)}}),
      notificationCallers_({argus::client::CallerCredential{
          .service = "argus-notification", .secret = std::move(input.notificationCallerSecret)}}),
      rooms_(input.rooms)
{
}

grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                        argus::voice::v1::ServerFrame>*
VoiceRpcService::Connect(grpc::CallbackServerContext* context)
{
  auto* reactor = new VoiceSessionStream(
      {.sessions = sessions_, .context = context, .callers = syncCallers_, .live = live_});
  reactor->begin();
  return reactor;
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
