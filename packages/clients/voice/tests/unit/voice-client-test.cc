#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/voice/v1/voice.grpc.pb.h>
#include <chrono>
#include <condition_variable>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <voice/voice-client.hxx>

namespace
{
namespace v1 = argus::voice::v1;

class CollectingObserver final : public VoiceStreamObserver
{
public:
  void onServerFrame(v1::ServerFrame frame) override
  {
    std::scoped_lock lock(mutex);
    frames.push_back(std::move(frame));
  }
  void onStreamClosed(const grpc::Status& status) override
  {
    std::scoped_lock lock(mutex);
    closed = true;
    closeStatus = status;
    cv.notify_all();
  }

  bool waitClosed(int timeoutMs)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                       [this] { return closed; });
  }

  std::vector<v1::ServerFrame> frames;
  bool closed{false};
  grpc::Status closeStatus;
  std::mutex mutex;
  std::condition_variable cv;
};

class RecordingVoiceService final : public v1::VoiceService::Service
{
public:
  using FrameStream =
      grpc::ServerReaderWriter<v1::ServerFrame, v1::ClientFrame>;

  grpc::Status Connect(grpc::ServerContext* context,
                       FrameStream* stream) override
  {
    {
      std::scoped_lock lock(mutex);
      context_ = context;
      for (const auto& [key, value] : context->client_metadata())
        metadata.emplace(std::string(key.begin(), key.end()),
                         std::string(value.begin(), value.end()));
    }
    v1::ClientFrame frame;
    bool answered = false;
    while (stream->Read(&frame)) {
      const bool started = frame.has_start();
      {
        std::scoped_lock lock(mutex);
        frames.push_back(frame);
      }
      if (started && !answered) {
        answered = true;
        v1::ServerFrame done;
        done.mutable_done()->set_session_id(4242);
        stream->Write(done);
      }
    }
    {
      std::scoped_lock lock(mutex);
      context_ = nullptr;
    }
    return grpc::Status::OK;
  }

  grpc::Status JoinRoom(grpc::ServerContext* context, const v1::RtcJoin* request,
                        v1::RtcJoined* reply) override
  {
    std::scoped_lock lock(mutex);
    credential = credentialOf(*context);
    joins.push_back(*request);
    reply->set_joined(true);
    reply->set_already(joins.size() > 1);
    return grpc::Status::OK;
  }

  grpc::Status Announce(grpc::ServerContext* context, const v1::AnnounceRequest* request,
                        v1::AnnounceResponse* reply) override
  {
    std::scoped_lock lock(mutex);
    credential = credentialOf(*context);
    announcements.push_back(*request);
    if (request->user_id() == 0)
      return {grpc::StatusCode::INVALID_ARGUMENT, "no user"};
    reply->set_delivered(request->user_id() == 7);
    return grpc::Status::OK;
  }

  static std::string credentialOf(const grpc::ServerContext& context)
  {
    for (const auto& [key, value] : context.client_metadata())
      if (std::string(key.begin(), key.end()) == "x-argus-credential")
        return {value.begin(), value.end()};
    return {};
  }

  void cancelActiveCall()
  {
    std::scoped_lock lock(mutex);
    if (context_ != nullptr)
      context_->TryCancel();
  }

  std::vector<v1::ClientFrame> frames;
  std::vector<v1::RtcJoin> joins;
  std::vector<v1::AnnounceRequest> announcements;
  std::string credential;
  std::map<std::string, std::string> metadata;
  std::mutex mutex;
  grpc::ServerContext* context_{nullptr};
};

std::unique_ptr<grpc::Server> startServer(RecordingVoiceService& service,
                                          int& port)
{
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  return std::unique_ptr<grpc::Server>(builder.BuildAndStart());
}

struct CallCleanup
{
  RecordingVoiceService& service;
  grpc::Server& server;

  ~CallCleanup()
  {
    service.cancelActiveCall();
    server.Shutdown(std::chrono::system_clock::now() +
                    std::chrono::milliseconds(500));
  }
};
}

TEST_CASE("the role string is the one a receiver gates on")
{
  CHECK(voiceRoleToString(v1::VOICE_ROLE_OWNER) == "owner");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_RESIDENT) == "resident");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_GUARD) == "guard");
  CHECK(voiceRoleToString(v1::VOICE_ROLE_GUEST) == "guest");
  CHECK(voiceRoleToString(static_cast<v1::VoiceRole>(42)) == "guest");
}

TEST_CASE("one stream carries the connect identity and the frames in order")
{
  RecordingVoiceService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  VoiceClient client(
      {.target = "127.0.0.1:" + std::to_string(port), .credential = ""});
  CHECK(client.waitConnected(5000));
  CallCleanup cleanup{service, *server};

  const auto observer = std::make_shared<CollectingObserver>();

  v1::VoiceIdentity connectIdentity;
  connectIdentity.set_user_id(7);
  connectIdentity.set_role(v1::VOICE_ROLE_RESIDENT);
  const auto stream = client.connect(connectIdentity, observer);

  v1::VoiceStart start;
  start.mutable_identity()->set_user_id(9);
  start.mutable_identity()->set_name("Ana");
  start.mutable_identity()->set_role(v1::VOICE_ROLE_OWNER);
  start.set_mode(v1::VOICE_MODE_DUPLEX);
  stream->start(start);
  stream->sendPcm("a\0b", 3);
  stream->skip();
  v1::VoiceActionResult result;
  result.set_id(3);
  result.set_ok(false);
  result.set_detail("sin permiso");
  stream->sendActionResult(result);
  stream->sendMute(true);
  stream->stop();
  stream->finish();
  REQUIRE(observer->waitClosed(5000));

  CHECK(observer->closeStatus.ok());
  REQUIRE(observer->frames.size() == 1);
  CHECK(observer->frames[0].done().session_id() == 4242);

  std::scoped_lock lock(service.mutex);
  CHECK(service.metadata.at("x-argus-user") == "7");
  CHECK(service.metadata.at("x-argus-role") == "resident");
  CHECK(service.metadata.count("x-argus-device") == 0);
  CHECK(service.metadata.count("x-argus-credential") == 0);
  REQUIRE(service.frames.size() == 6);
  REQUIRE(service.frames[0].has_start());
  CHECK(service.frames[0].start().identity().user_id() == 9);
  CHECK(service.frames[0].start().identity().role() == v1::VOICE_ROLE_OWNER);
  CHECK(service.frames[0].start().mode() == v1::VOICE_MODE_DUPLEX);
  CHECK(service.frames[1].pcm() == std::string("a\0b", 3));
  CHECK(service.frames[2].has_skip());
  REQUIRE(service.frames[3].has_action_result());
  CHECK(service.frames[3].action_result().id() == 3);
  CHECK_FALSE(service.frames[3].action_result().ok());
  CHECK(service.frames[3].action_result().detail() == "sin permiso");
  REQUIRE(service.frames[4].has_mute());
  CHECK(service.frames[4].mute().muted());
  CHECK(service.frames[5].has_stop());
}

TEST_CASE("joinRoom carries the whole join and the caller credential, and reports a second join")
{
  RecordingVoiceService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  VoiceClient client({.target = "127.0.0.1:" + std::to_string(port), .credential = "sync-secret"});
  CallCleanup cleanup{service, *server};

  v1::RtcJoin join;
  join.set_room("u7.rtc-0123");
  join.set_agent_token("agent.jwt");
  join.set_user_identity("user:7:abcd");
  join.mutable_identity()->set_user_id(7);
  join.set_mode(v1::VOICE_MODE_DUPLEX);
  join.set_call_id("rtc-0123");
  join.set_opening_line("Hola");
  const VoiceRoomJoinResult first = client.joinRoom(join);
  const VoiceRoomJoinResult second = client.joinRoom(join);

  CHECK(first.status.ok());
  CHECK(first.joined);
  CHECK_FALSE(first.already);
  CHECK(second.already);
  std::scoped_lock lock(service.mutex);
  CHECK(service.credential == "sync-secret");
  REQUIRE(service.joins.size() == 2);
  CHECK(service.joins[0].room() == "u7.rtc-0123");
  CHECK(service.joins[0].agent_token() == "agent.jwt");
  CHECK(service.joins[0].user_identity() == "user:7:abcd");
  CHECK(service.joins[0].opening_line() == "Hola");
}

TEST_CASE("announce answers delivered, not delivered, or nothing when the call fails")
{
  RecordingVoiceService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  VoiceClient client({.target = "127.0.0.1:" + std::to_string(port), .credential = "n-secret"});
  CallCleanup cleanup{service, *server};

  CHECK(client.announce({.userId = 7, .text = "Ha llegado alguien.", .kind = "guard_episode", .callId = "call-3"}) == true);
  CHECK(client.announce({.userId = 8, .text = "x", .kind = "agenda", .callId = "call-4"}) == false);
  CHECK_FALSE(client.announce({.userId = 0, .text = "x", .kind = "agenda", .callId = ""}).has_value());
  std::scoped_lock lock(service.mutex);
  CHECK(service.credential == "n-secret");
  REQUIRE(service.announcements.size() == 3);
  CHECK(service.announcements[0].text() == "Ha llegado alguien.");
  CHECK(service.announcements[0].call_id() == "call-3");
}

TEST_CASE("joinRoom against nothing listening fails within its deadline")
{
  VoiceClient client({.target = "127.0.0.1:1", .credential = ""});
  const auto started = std::chrono::steady_clock::now();
  const VoiceRoomJoinResult result = client.joinRoom(v1::RtcJoin{});
  CHECK_FALSE(result.status.ok());
  CHECK_FALSE(result.joined);
  CHECK(std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(VoiceClient::kJoinRoomDeadlineMs + 1000));
}

TEST_CASE("a user role and a language reach the voice wire as their proto values")
{
  CHECK(voiceRoleToProto(UserRole::Owner) == v1::VOICE_ROLE_OWNER);
  CHECK(voiceRoleToProto(UserRole::Resident) == v1::VOICE_ROLE_RESIDENT);
  CHECK(voiceRoleToProto(UserRole::Guard) == v1::VOICE_ROLE_GUARD);
  CHECK(voiceRoleToProto(UserRole::Guest) == v1::VOICE_ROLE_GUEST);
  CHECK(voiceLanguageToProto("es") == v1::VOICE_LANGUAGE_ES);
  CHECK(voiceLanguageToProto("en") == v1::VOICE_LANGUAGE_EN);
  CHECK(voiceLanguageToProto("fr") == v1::VOICE_LANGUAGE_SYSTEM);
  CHECK(voiceLanguageToProto("") == v1::VOICE_LANGUAGE_SYSTEM);
}
