#include "voice-rpc-service.hxx"

#include <condition_variable>
#include <deque>
#include <drogon/drogon.h>

namespace
{

// One bidi stream; doubles as the session's VoiceSessionSink.
class VoiceSessionStream final
    : public grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                                     argus::voice::v1::ServerFrame>,
      public VoiceSessionSink
{
public:
  VoiceSessionStream(VoiceSessionService& sessions,
                     grpc::CallbackServerContext* context)
      : sessions_(sessions), context_(context)
  {
  }

  void begin()
  {
    if (!authorized()) {
      Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                          "identity metadata missing"));
      return;
    }
    StartRead(&read_);
  }

  void OnReadDone(bool ok) override
  {
    if (!ok) {
      endSession();
      return;
    }
    handleFrame(std::move(read_));
    read_ = {};
    StartRead(&read_);
  }

  void OnWriteDone(bool ok) override
  {
    std::lock_guard<std::mutex> lock(writeMutex_);
    writing_ = false;
    if (!ok) {
      finished_ = true;
      queue_.clear();
    }
    else if (!queue_.empty())
      queue_.pop_front();
    pumpLocked();
    drainCv_.notify_all();
  }

  void OnDone() override { endSession(); }

  bool connected() const override
  {
    return !finished_ && !context_->IsCancelled();
  }

  void sendServerFrame(argus::voice::v1::ServerFrame frame) override
  {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (finished_)
      return;
    if (queue_.size() >= kMaxPendingWrites) {
      LOG_WARN << "Voice: stream write backlog, dropping frame";
      return;
    }
    queue_.push_back(std::move(frame));
    pumpLocked();
  }

private:
  // Metadata presence only; roles were validated at the gateway.
  bool authorized() const
  {
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

  void handleFrame(argus::voice::v1::ClientFrame frame)
  {
    switch (frame.body_case()) {
      case argus::voice::v1::ClientFrame::kStart:
        sessions_.start(*this, frame.start().identity());
        break;
      case argus::voice::v1::ClientFrame::kStop:
        sessions_.stop(*this);
        break;
      case argus::voice::v1::ClientFrame::kSkip:
        sessions_.skip(*this);
        break;
      case argus::voice::v1::ClientFrame::kPcm:
        sessions_.feedPcm(*this, frame.pcm().data(),
                          static_cast<size_t>(frame.pcm().size()));
        break;
      default:
        LOG_WARN << "Voice: client frame without body";
        break;
    }
  }

  // Ends the session and closes the RPC; idempotent.
  void endSession()
  {
    sessions_.stop(*this);
    drain();
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (finishing_ || context_->IsCancelled())
      return;
    finishing_ = true;
    Finish(grpc::Status::OK);
  }

  // Bounded wait so voice:done is not cut off by Finish.
  void drain()
  {
    std::unique_lock<std::mutex> lock(writeMutex_);
    drainCv_.wait_for(lock, std::chrono::seconds(2), [this] {
      return queue_.empty() || finished_;
    });
  }

  void pumpLocked()
  {
    if (writing_ || queue_.empty() || finished_)
      return;
    writing_ = true;
    StartWrite(&queue_.front());
  }

  // Backpressure cap.
  static constexpr size_t kMaxPendingWrites = 512;

  VoiceSessionService& sessions_;
  grpc::CallbackServerContext* context_;
  std::mutex writeMutex_;
  std::condition_variable drainCv_;
  std::deque<argus::voice::v1::ServerFrame> queue_;
  bool writing_{false};
  bool finished_{false};
  bool finishing_{false};
  argus::voice::v1::ClientFrame read_;
};

} // namespace

grpc::ServerBidiReactor<argus::voice::v1::ClientFrame,
                        argus::voice::v1::ServerFrame>*
VoiceRpcService::Connect(grpc::CallbackServerContext* context)
{
  auto* reactor = new VoiceSessionStream(sessions_, context);
  reactor->begin();
  return reactor;
}
