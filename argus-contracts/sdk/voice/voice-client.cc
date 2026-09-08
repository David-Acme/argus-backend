#include "voice-client.hxx"

#include <chrono>
#include <deque>
#include <mutex>

namespace
{
// Keepalive probes keep idle voice streams alive and detect a dead peer.
constexpr int kKeepaliveTimeMs = 30000;
constexpr int kKeepaliveTimeoutMs = 10000;
constexpr int kMaxPingsWithoutData = 0;

// One write in flight; frames beyond the cap are dropped.
constexpr size_t kMaxPendingWrites = 1024;

class VoiceStreamImpl final
    : public VoiceStream,
      public grpc::ClientBidiReactor<argus::voice::v1::ClientFrame,
                                     argus::voice::v1::ServerFrame>
{
public:
  VoiceStreamImpl(argus::voice::v1::VoiceService::StubInterface* stub,
                  std::unique_ptr<grpc::ClientContext> context,
                  std::shared_ptr<VoiceStreamObserver> observer,
                  argus::voice::v1::VoiceIdentity identity)
      : stub_(stub), context_(std::move(context)),
        observer_(std::move(observer)), identity_(std::move(identity))
  {
  }

  void start(const argus::voice::v1::VoiceIdentity& identity) override
  {
    argus::voice::v1::ClientFrame frame;
    *frame.mutable_start()->mutable_identity() = identity;
    writeFrame(std::move(frame));
  }

  void stop() override
  {
    argus::voice::v1::ClientFrame frame;
    frame.mutable_stop();
    writeFrame(std::move(frame));
  }

  void skip() override
  {
    argus::voice::v1::ClientFrame frame;
    frame.mutable_skip();
    writeFrame(std::move(frame));
  }

  void sendPcm(const void* data, size_t size) override
  {
    argus::voice::v1::ClientFrame frame;
    frame.set_pcm(static_cast<const char*>(data), size);
    writeFrame(std::move(frame));
  }

  void finish() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_ || writesDone_)
      return;
    writesDone_ = true;
    StartWritesDone();
  }

  void OnReadDone(bool ok) override
  {
    if (!ok)
      return;
    std::shared_ptr<VoiceStreamObserver> observer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observer = observer_;
    }
    if (!observer)
      return;
    observer->onServerFrame(std::move(read_));
    read_ = {};
    StartRead(&read_);
  }

  void OnWriteDone(bool ok) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
      pending_.clear();
      writing_ = false;
      return;
    }
    if (!pending_.empty())
      pending_.pop_front();
    if (!writesDone_ && pending_.empty()) {
      writing_ = false;
      return;
    }
    drainLocked();
  }

  void OnDone(const grpc::Status& status) override
  {
    std::shared_ptr<VoiceStreamImpl> self;
    std::shared_ptr<VoiceStreamObserver> observer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
      self = std::move(self_);
      observer = observer_;
    }
    if (observer)
      observer->onStreamClosed(status);
  }

  void begin()
  {
    context_->AddMetadata("x-argus-user",
                          std::to_string(identity_.user_id()));
    context_->AddMetadata("x-argus-role", voiceRoleToString(identity_.role()));

    {
      std::lock_guard<std::mutex> lock(mutex_);
      self_ = std::shared_ptr<VoiceStreamImpl>(this, [](VoiceStreamImpl*) {});
    }
    stub_->async()->Connect(context_.get(), this);
    StartCall();
    StartRead(&read_);
  }

private:
  void writeFrame(argus::voice::v1::ClientFrame frame)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_ || writesDone_)
      return;
    if (pending_.size() >= kMaxPendingWrites)
      return;
    pending_.push_back(std::move(frame));
    drainLocked();
  }

  void drainLocked()
  {
    if (writing_ || pending_.empty() || writesDone_)
      return;
    writing_ = true;
    StartWrite(&pending_.front());
  }

  argus::voice::v1::VoiceService::StubInterface* stub_;
  std::unique_ptr<grpc::ClientContext> context_;
  std::shared_ptr<VoiceStreamObserver> observer_;
  argus::voice::v1::VoiceIdentity identity_;

  std::shared_ptr<VoiceStreamImpl> self_;

  std::mutex mutex_;
  std::deque<argus::voice::v1::ClientFrame> pending_;
  bool writing_{false};
  bool writesDone_{false};
  bool done_{false};
  argus::voice::v1::ServerFrame read_;
};

} // namespace

VoiceClient::VoiceClient(std::string target)
{
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, kKeepaliveTimeMs);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, kKeepaliveTimeoutMs);
  args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, kMaxPingsWithoutData);
  channel_ = grpc::CreateCustomChannel(target,
                                       grpc::InsecureChannelCredentials(),
                                       args);
  stub_ = argus::voice::v1::VoiceService::NewStub(channel_);
}

std::shared_ptr<VoiceStream> VoiceClient::connect(
    const argus::voice::v1::VoiceIdentity& identity,
    std::shared_ptr<VoiceStreamObserver> observer)
{
  auto context = std::make_unique<grpc::ClientContext>();
  auto stream = std::shared_ptr<VoiceStreamImpl>(
      new VoiceStreamImpl(stub_.get(), std::move(context),
                          std::move(observer), identity));
  stream->begin();
  return stream;
}

bool VoiceClient::waitConnected(int timeoutMs) const
{
  return channel_->WaitForConnected(
      std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs));
}

std::string voiceRoleToString(argus::voice::v1::VoiceRole role)
{
  switch (role) {
    case argus::voice::v1::VOICE_ROLE_OWNER:
      return "owner";
    case argus::voice::v1::VOICE_ROLE_RESIDENT:
      return "resident";
    case argus::voice::v1::VOICE_ROLE_GUARD:
      return "guard";
    case argus::voice::v1::VOICE_ROLE_GUEST:
      break;
  }
  return "guest";
}
