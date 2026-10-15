#include "voice-client.hxx"

#include <grpc/grpc-client-base.hxx>

#include "frame-queue.hxx"

#include <chrono>
#include <mutex>

namespace
{
struct VoiceStreamInput
{
  argus::voice::v1::VoiceService::StubInterface* stub;
  std::unique_ptr<grpc::ClientContext> context;
  std::shared_ptr<VoiceStreamObserver> observer;
  argus::voice::v1::VoiceIdentity identity;
};

class VoiceStreamImpl final
    : public VoiceStream,
      public grpc::ClientBidiReactor<argus::voice::v1::ClientFrame,
                                     argus::voice::v1::ServerFrame>,
      public std::enable_shared_from_this<VoiceStreamImpl>
{
public:
  explicit VoiceStreamImpl(VoiceStreamInput input)
      : stub_(input.stub), context_(std::move(input.context)),
        observer_(std::move(input.observer)),
        identity_(std::move(input.identity))
  {
  }

  void start(const argus::voice::v1::VoiceStart& start) override
  {
    argus::voice::v1::ClientFrame frame;
    *frame.mutable_start() = start;
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

  void sendContext(const argus::voice::v1::VoiceContext& context) override
  {
    argus::voice::v1::ClientFrame frame;
    *frame.mutable_context() = context;
    writeFrame(std::move(frame));
  }

  void sendActionResult(const argus::voice::v1::VoiceActionResult& result) override
  {
    argus::voice::v1::ClientFrame frame;
    *frame.mutable_action_result() = result;
    writeFrame(std::move(frame));
  }

  void sendMute(bool muted) override
  {
    argus::voice::v1::ClientFrame frame;
    frame.mutable_mute()->set_muted(muted);
    writeFrame(std::move(frame));
  }

  void sendFarewell(const std::string& reason) override
  {
    argus::voice::v1::ClientFrame frame;
    frame.mutable_farewell()->set_reason(reason);
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
    std::scoped_lock lock(mutex_);
    if (done_ || queue_.cancelled())
      return;
    queue_.cancel();
    if (context_)
      context_->TryCancel();
  }

  void OnReadDone(bool ok) override
  {
    if (!ok)
      return;
    std::shared_ptr<VoiceStreamObserver> observer;
    {
      std::scoped_lock lock(mutex_);
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
    std::scoped_lock lock(mutex_);
    queue_.completeWrite(ok);
    drainLocked();
  }

  void OnDone(const grpc::Status& status) override
  {
    std::shared_ptr<VoiceStreamImpl> self;
    std::shared_ptr<VoiceStreamObserver> observer;
    {
      std::scoped_lock lock(mutex_);
      done_ = true;
      self = std::move(self_);
      observer = observer_;
    }
    if (observer)
      observer->onStreamClosed(status);
  }

  void begin()
  {
    argus::client::addCallerIdentity(
        *context_, {.userId = identity_.user_id(),
                    .role = identity_.has_role() ? voiceRoleToString(identity_.role())
                                                 : userRoleToString(UserRole::Unknown)});

    {
      std::scoped_lock lock(mutex_);
      self_ = shared_from_this();
    }
    stub_->async()->Connect(context_.get(), this);
    StartCall();
    StartRead(&read_);
  }

private:
  void writeFrame(argus::voice::v1::ClientFrame frame)
  {
    std::scoped_lock lock(mutex_);
    if (done_ || !queue_.push(std::move(frame)))
      return;
    drainLocked();
  }

  void drainLocked()
  {
    if (auto* next = queue_.nextWrite())
      StartWrite(next);
  }

  argus::voice::v1::VoiceService::StubInterface* stub_;
  std::unique_ptr<grpc::ClientContext> context_;
  std::shared_ptr<VoiceStreamObserver> observer_;
  argus::voice::v1::VoiceIdentity identity_;

  std::shared_ptr<VoiceStreamImpl> self_;

  std::mutex mutex_;
  FrameQueue queue_;
  bool done_{false};
  argus::voice::v1::ServerFrame read_;
};

}

VoiceClient::VoiceClient(VoiceClientConfig config)
    : channel_(argus::client::makeStreamingChannel(config.target)),
      stub_(argus::voice::v1::VoiceService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::shared_ptr<VoiceStream> VoiceClient::connect(
    const argus::voice::v1::VoiceIdentity& identity,
    std::shared_ptr<VoiceStreamObserver> observer)
{
  auto context = std::make_unique<grpc::ClientContext>();
  argus::client::addCallerCredential(*context, credential_);
  auto stream = std::shared_ptr<VoiceStreamImpl>(new VoiceStreamImpl(
      {.stub = stub_.get(),
       .context = std::move(context),
       .observer = std::move(observer),
       .identity = identity}));
  stream->begin();
  return stream;
}

bool VoiceClient::waitConnected(int timeoutMs) const
{
  return channel_->WaitForConnected(
      std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs));
}

VoiceRoomJoinResult VoiceClient::joinRoom(const argus::voice::v1::RtcJoin& join) const
{
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, credential_);
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(kJoinRoomDeadlineMs));
  argus::voice::v1::RtcJoined reply;
  VoiceRoomJoinResult result;
  result.status = stub_->JoinRoom(&context, join, &reply);
  if (result.status.ok()) {
    result.joined = reply.joined();
    result.already = reply.already();
  }
  return result;
}

bool VoiceClient::farewell(const argus::voice::v1::RtcFarewell& farewell, int deadlineMs) const
{
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, credential_);
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
  argus::voice::v1::RtcFarewellDone reply;
  return stub_->Farewell(&context, farewell, &reply).ok();
}

std::optional<bool> VoiceClient::announce(const VoiceAnnounceInput& input) const
{
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, credential_);
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(kAnnounceDeadlineMs));
  argus::voice::v1::AnnounceRequest request;
  request.set_user_id(input.userId);
  request.set_text(input.text);
  request.set_kind(input.kind);
  request.set_call_id(input.callId);
  argus::voice::v1::AnnounceResponse reply;
  if (!stub_->Announce(&context, request, &reply).ok())
    return std::nullopt;
  return reply.delivered();
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
      return "guest";
    default:
      break;
  }
  return userRoleToString(UserRole::Unknown);
}

argus::voice::v1::VoiceRole voiceRoleToProto(UserRole role)
{
  switch (role) {
    case UserRole::Owner:
      return argus::voice::v1::VOICE_ROLE_OWNER;
    case UserRole::Resident:
      return argus::voice::v1::VOICE_ROLE_RESIDENT;
    case UserRole::Guard:
      return argus::voice::v1::VOICE_ROLE_GUARD;
    case UserRole::Guest:
      return argus::voice::v1::VOICE_ROLE_GUEST;
    case UserRole::Unknown:
      break;
  }
  return argus::voice::v1::VOICE_ROLE_UNKNOWN;
}

argus::voice::v1::VoiceLanguage voiceLanguageToProto(std::string_view lang)
{
  if (lang == "es")
    return argus::voice::v1::VOICE_LANGUAGE_ES;
  if (lang == "en")
    return argus::voice::v1::VOICE_LANGUAGE_EN;
  return argus::voice::v1::VOICE_LANGUAGE_SYSTEM;
}
