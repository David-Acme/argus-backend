#include "voice-grpc-relay.hxx"

#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <config/config-service.hxx>
#include <runtime/blocking-task.hxx>
#include <sync/sync-errors.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <voice/reaction-contracts.hxx>

#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace
{

constexpr int kConnectProbeTimeoutMs = 500;
constexpr std::size_t kMaxContextChars = 300;
constexpr std::size_t kMaxSituationChars = 900;
constexpr std::size_t kMaxResultDetailChars = 160;
constexpr std::size_t kMaxPendingOps = 16;

std::string utf8Prefix(const std::string& text, std::size_t maxBytes)
{
  if (text.size() <= maxBytes)
    return text;
  std::size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
    --end;
  return text.substr(0, end);
}

std::string stringField(const Json::Value& payload, const char* key, std::size_t maxBytes)
{
  const Json::Value& value = payload[key];
  return value.isString() ? utf8Prefix(value.asString(), maxBytes) : std::string();
}

argus::voice::v1::VoiceContextKind contextKindOf(const Json::Value& payload)
{
  const Json::Value& kind = payload["kind"];
  if (!kind.isString())
    return argus::voice::v1::VOICE_CONTEXT_NOTE;
  if (kind.asString() == "cameraEvent")
    return argus::voice::v1::VOICE_CONTEXT_CAMERA_EVENT;
  if (kind.asString() == "situation")
    return argus::voice::v1::VOICE_CONTEXT_SITUATION;
  return argus::voice::v1::VOICE_CONTEXT_NOTE;
}

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

argus::voice::v1::VoiceContext VoiceGrpcRelay::contextOf(const Json::Value& payload)
{
  argus::voice::v1::VoiceContext context;
  if (!payload.isObject())
    return context;
  context.set_kind(contextKindOf(payload));
  const std::size_t textLimit = context.kind() == argus::voice::v1::VOICE_CONTEXT_SITUATION
                                    ? kMaxSituationChars
                                    : kMaxContextChars;
  context.set_text(stringField(payload, "text", textLimit));
  context.set_camera(stringField(payload, "camera", kMaxContextChars));
  return context;
}

argus::voice::v1::VoiceActionResult VoiceGrpcRelay::actionResultOf(const Json::Value& payload)
{
  argus::voice::v1::VoiceActionResult result;
  if (!payload.isObject())
    return result;
  const Json::Value& id = payload["id"];
  if (id.isIntegral())
    result.set_id(id.asInt64());
  else if (id.isString()) {
    const std::string& raw = id.asString();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (error == std::errc{} && end == raw.data() + raw.size())
      result.set_id(parsed);
  }
  result.set_ok(payload["ok"].isBool() && payload["ok"].asBool());
  result.set_detail(stringField(payload, "detail", kMaxResultDetailChars));
  return result;
}

bool VoiceGrpcRelay::mutedOf(const Json::Value& payload)
{
  return payload.isObject() && payload["muted"].isBool() && payload["muted"].asBool();
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
  else if (frame.has_action()) {
    msg["type"] = "voice:action";
    payload["id"] = static_cast<Json::Int64>(frame.action().id());
    payload["name"] = frame.action().name();
    Json::Value arguments = json_util::fromString(frame.action().arguments());
    if (!arguments.isObject())
      arguments = Json::Value(Json::objectValue);
    payload["arguments"] = arguments;
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

bool VoiceGrpcRelay::resumeOf(const Json::Value& message)
{
  if (!message.isObject() || !message["payload"].isObject())
    return false;
  const Json::Value& resume = message["payload"]["resume"];
  return resume.isBool() && resume.asBool();
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
      {
        std::scoped_lock lock(session->mutex);
        session->stream.reset();
      }
      if (session->closing || conn->disconnected())
        return;
      if (status.ok())
        return;
      LOG_WARN << "Voice relay: stream failed: " << status.error_message();
      const std::string type = "voice:start";
      const std::string error(SyncErrors::VoiceUnavailable.message);
      sendSocketFrameError({.conn = conn, .type = type, .status = 503, .error = error});
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
    session->deviceHash = ctx.deviceHash;
  }

  std::scoped_lock lock(sessionsMutex_);
  sessions_[conn.get()] = std::move(session);
}

std::shared_ptr<VoiceGrpcRelay::Session>
VoiceGrpcRelay::sessionFor(const drogon::WebSocketConnectionPtr& conn)
{
  std::scoped_lock lock(sessionsMutex_);
  auto it = sessions_.find(conn.get());
  return it == sessions_.end() ? nullptr : it->second;
}

std::shared_ptr<VoiceGrpcRelay::Session>
VoiceGrpcRelay::takeSession(const drogon::WebSocketConnectionPtr& conn)
{
  std::scoped_lock lock(sessionsMutex_);
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
  if (session->closing)
    co_return true;

  if (type == "voice:start") {
    co_await startStream(
        {.conn = conn, .session = session, .mode = startModeOf(message), .resume = resumeOf(message)});
    co_return true;
  }

  if (type == "voice:stop") {
    deliver(*session, [](VoiceStream& stream) { stream.stop(); });
    co_return true;
  }

  if (type == "voice:skip") {
    deliver(*session, [](VoiceStream& stream) { stream.skip(); });
    co_return true;
  }

  if (type == "voice:context") {
    deliver(*session, [context = contextOf(message["payload"])](VoiceStream& stream) {
      stream.sendContext(context);
    });
    co_return true;
  }

  if (type == "voice:action_result") {
    deliver(*session, [result = actionResultOf(message["payload"])](VoiceStream& stream) {
      stream.sendActionResult(result);
    });
    co_return true;
  }

  if (type == "voice:mute") {
    deliver(*session, [muted = mutedOf(message["payload"])](VoiceStream& stream) {
      stream.sendMute(muted);
    });
    co_return true;
  }

  co_return false;
}

void VoiceGrpcRelay::deliver(Session& session, StreamOp op)
{
  std::shared_ptr<VoiceStream> stream;
  {
    std::scoped_lock lock(session.mutex);
    stream = session.stream;
    if (!stream) {
      if (session.starting && session.pending.size() < kMaxPendingOps)
        session.pending.push_back(std::move(op));
      return;
    }
  }
  op(*stream);
}

drogon::Task<void> VoiceGrpcRelay::startStream(StartInput input)
{
  const auto session = input.session;
  const auto conn = input.conn;
  const auto mode = input.mode;
  const bool resume = input.resume;
  std::shared_ptr<VoiceStream> stream;
  {
    std::scoped_lock lock(session->mutex);
    stream = session->stream;
    if (!stream)
      session->starting = true;
  }

  std::optional<DirectoryUser> user;
  if (userDirectory_)
    user = co_await userDirectory_->findById(session->userId);
  argus::voice::v1::VoiceStart start;
  argus::voice::v1::VoiceIdentity& identity = *start.mutable_identity();
  identity.set_user_id(session->userId);
  identity.set_role(voiceRoleToProto(session->role));
  identity.set_device_hash(session->deviceHash);
  if (user) {
    identity.set_name(user->name);
    identity.set_language(voiceLangToProto(user->lang));
  }
  start.set_mode(mode);
  start.set_resume(resume);

  if (stream) {
    stream->start(start);
    co_return;
  }

  const bool up = co_await BlockingTask<bool>{
      [this] { return client_->waitConnected(kConnectProbeTimeoutMs); }};
  std::vector<StreamOp> pending;
  {
    std::scoped_lock lock(session->mutex);
    session->starting = false;
    pending.swap(session->pending);
    if (up && !session->closing && !session->stream)
      session->stream = client_->connect(
          start.identity(), std::make_shared<StreamObserver>(conn, session));
    stream = session->stream;
  }
  if (!up)
    throw ResponseException(503, SyncErrors::VoiceUnavailable);
  if (!stream)
    co_return;
  stream->start(start);
  for (auto& op : pending)
    op(*stream);
}

void VoiceGrpcRelay::forwardBinary(const drogon::WebSocketConnectionPtr& conn,
                                   const std::string& data)
{
  auto session = sessionFor(conn);
  if (!session || session->closing)
    return;
  std::shared_ptr<VoiceStream> stream;
  {
    std::scoped_lock lock(session->mutex);
    stream = session->stream;
  }
  if (stream)
    stream->sendPcm(data.data(), data.size());
}

void VoiceGrpcRelay::onClose(const drogon::WebSocketConnectionPtr& conn)
{
  auto session = takeSession(conn);
  if (!session)
    return;
  session->closing = true;
  std::shared_ptr<VoiceStream> stream;
  {
    std::scoped_lock lock(session->mutex);
    stream = std::move(session->stream);
    session->pending.clear();
  }
  if (stream)
    stream->finish();
}
