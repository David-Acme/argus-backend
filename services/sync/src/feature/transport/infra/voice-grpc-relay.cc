#include "voice-grpc-relay.hxx"

#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <config/config-service.hxx>
#include <runtime/blocking-task.hxx>
#include <sync/sync-errors.hxx>
#include <trantor/utils/Logger.h>
#include <voice/reaction-contracts.hxx>

#include <cstdint>
#include <optional>
#include <utility>

namespace
{

constexpr int kConnectProbeTimeoutMs = 500;

ReactionKind reactionKindFromProto(argus::voice::v1::ReactionKind reaction)
{
  return static_cast<ReactionKind>(reaction);
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
      break;
  }
  return argus::voice::v1::VOICE_ROLE_GUEST;
}

argus::voice::v1::VoiceLanguage voiceLangToProto(const std::string& lang)
{
  if (lang == "es")
    return argus::voice::v1::VOICE_LANGUAGE_ES;
  if (lang == "en")
    return argus::voice::v1::VOICE_LANGUAGE_EN;
  return argus::voice::v1::VOICE_LANGUAGE_SYSTEM;
}

}

VoiceGrpcConfig VoiceGrpcConfig::resolve()
{
  VoiceGrpcConfig config;
  config.target = ConfigService::getString("voice.target");
  config.credential = ConfigService::getString("voice.credential");
  return config;
}

Json::Value VoiceGrpcRelay::renderServerFrame(
    const argus::voice::v1::ServerFrame& frame)
{
  Json::Value msg(Json::objectValue);
  Json::Value payload(Json::objectValue);
  if (frame.has_stt()) {
    msg["type"] = "voice:stt";
    payload["text"] = frame.stt().text();
    payload["final"] = frame.stt().final();
  }
  else if (frame.has_assistant()) {
    msg["type"] = "voice:assistant";
    payload["text"] = frame.assistant().text();
    if (frame.assistant().turn_id() != 0)
      payload["turnId"] = static_cast<Json::Int64>(frame.assistant().turn_id());
  }
  else if (frame.has_event()) {
    msg["type"] = "voice:event";
    payload["reaction"] =
        reactionKindToString(reactionKindFromProto(frame.event().reaction()));
    payload["intensity"] = frame.event().intensity();
    payload["because"] = frame.event().because();
  }
  else if (frame.has_done()) {
    msg["type"] = "voice:done";
    payload["sessionId"] = Json::Int64(frame.done().session_id());
  }
  else if (frame.has_turn()) {
    msg["type"] = "voice:turn";
    payload["id"] = static_cast<Json::Int64>(frame.turn().id());
  }
  else if (frame.has_interrupted()) {
    msg["type"] = "voice:interrupted";
    payload["id"] = static_cast<Json::Int64>(frame.interrupted().id());
  }
  msg["payload"] = payload;
  return msg;
}

argus::voice::v1::VoiceMode VoiceGrpcRelay::startModeOf(
    const Json::Value& message)
{
  if (!message.isObject())
    return argus::voice::v1::VOICE_MODE_HALF_DUPLEX;
  const Json::Value& payload = message["payload"];
  if (!payload.isObject())
    return argus::voice::v1::VOICE_MODE_HALF_DUPLEX;
  const Json::Value& mode = payload["mode"];
  if (mode.isString() && mode.asString() == "duplex")
    return argus::voice::v1::VOICE_MODE_DUPLEX;
  return argus::voice::v1::VOICE_MODE_HALF_DUPLEX;
}

class VoiceGrpcRelay::StreamObserver final : public VoiceStreamObserver
{
public:
  StreamObserver(drogon::WebSocketConnectionPtr conn,
                 std::shared_ptr<Session> session)
      : conn_(std::move(conn)), session_(std::move(session))
  {
  }

  void onServerFrame(argus::voice::v1::ServerFrame frame) override
  {
    const auto loop = session_->loop;
    if (!loop)
      return;
    loop->queueInLoop(
        [conn = conn_, session = session_, frame = std::move(frame)]() {
          if (session->closing || conn->disconnected())
            return;
          if (frame.has_tts_chunk()) {
            const auto& pcm = frame.tts_chunk().pcm();
            conn->send(pcm.data(), static_cast<uint64_t>(pcm.size()),
                       drogon::WebSocketMessageType::Binary);
            return;
          }
          conn->sendJson(renderServerFrame(frame));
        });
  }

  void onStreamClosed(const grpc::Status& status) override
  {
    const auto loop = session_->loop;
    if (!loop)
      return;
    loop->queueInLoop([conn = conn_, session = session_, status]() {
      session->stream.reset();
      if (session->closing || conn->disconnected())
        return;
      if (status.ok())
        return;
      LOG_WARN << "Voice relay: stream failed: " << status.error_message();
      session->failed = true;
      conn->shutdown(drogon::CloseCode::kNormalClosure);
    });
  }

private:
  drogon::WebSocketConnectionPtr conn_;
  std::shared_ptr<Session> session_;
};

VoiceGrpcRelay::VoiceGrpcRelay(
    VoiceGrpcConfig config, std::shared_ptr<const IUserDirectory> directory)
    : client_(std::make_shared<VoiceClient>(
          VoiceClientConfig{.target = std::move(config.target),
                            .credential = std::move(config.credential)})),
      userDirectory_(std::move(directory))
{
}

void VoiceGrpcRelay::onConnect(const drogon::HttpRequestPtr& req,
                               const drogon::WebSocketConnectionPtr& conn)
{
  auto session = std::make_shared<Session>();
  session->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  if (!session->loop)
    session->loop = drogon::app().getIOLoop(0);

  if (req->getAttributes()->find(AuthContext::kJwtKey)) {
    const auto& ctx =
        req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
    session->userId = ctx.sub;
    session->role = ctx.role;
  }

  std::lock_guard<std::mutex> lock(sessionsMutex_);
  sessions_[conn.get()] = std::move(session);
}

std::shared_ptr<VoiceGrpcRelay::Session>
VoiceGrpcRelay::sessionFor(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  return it == sessions_.end() ? nullptr : it->second;
}

std::shared_ptr<VoiceGrpcRelay::Session>
VoiceGrpcRelay::takeSession(const drogon::WebSocketConnectionPtr& conn)
{
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  if (it == sessions_.end())
    return nullptr;
  auto session = std::move(it->second);
  sessions_.erase(it);
  return session;
}

drogon::Task<bool> VoiceGrpcRelay::forwardText(const SyncFrameInput& input)
{
  const drogon::WebSocketConnectionPtr& conn = input.conn;
  const Json::Value& message = input.message;
  static_cast<void>(input.raw);
  const std::string type = message["type"].asString();
  auto session = sessionFor(conn);
  if (!session)
    co_return false;
  if (session->failed)
    throw ResponseException(503, SyncErrors::VoiceUnavailable);
  if (session->closing)
    co_return true;

  if (type == "voice:start") {
    if (session->stream)
      co_return true;

    std::optional<DirectoryUser> user;
    if (userDirectory_)
      user = co_await userDirectory_->findById(session->userId);
    argus::voice::v1::VoiceIdentity identity;
    identity.set_user_id(session->userId);
    identity.set_role(voiceRoleToProto(session->role));
    if (user) {
      identity.set_name(user->name);
      identity.set_language(voiceLangToProto(user->lang));
    }

    const bool up = co_await BlockingTask<bool>{
        [this] { return client_->waitConnected(kConnectProbeTimeoutMs); }};
    if (!up)
      throw ResponseException(503, SyncErrors::VoiceUnavailable);
    if (session->closing || session->stream)
      co_return true;

    argus::voice::v1::VoiceStart start;
    *start.mutable_identity() = identity;
    start.set_mode(startModeOf(message));
    session->stream = client_->connect(
        identity, std::make_shared<StreamObserver>(conn, session));
    session->stream->start(start);
    co_return true;
  }

  if (type == "voice:stop") {
    if (session->stream)
      session->stream->stop();
    co_return true;
  }

  if (type == "voice:skip") {
    if (session->stream)
      session->stream->skip();
    co_return true;
  }

  co_return false;
}

void VoiceGrpcRelay::forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                                   const std::string& data)
{
  auto session = sessionFor(conn);
  if (!session || session->failed || session->closing)
    return;
  if (!session->stream)
    return;
  session->stream->sendPcm(data.data(), data.size());
}

void VoiceGrpcRelay::onClose(const drogon::WebSocketConnectionPtr& conn)
{
  auto session = takeSession(conn);
  if (!session)
    return;
  session->closing = true;
  if (session->stream)
    session->stream->finish();
}
