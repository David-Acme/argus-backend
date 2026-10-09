#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/voice-rpc-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr const char* kSyncSecret = "voice-rpc-auth-test-secret";
constexpr const char* kNotificationSecret = "voice-rpc-auth-test-notification";

class RecordingJoiner final : public VoiceRoomJoiner
{
public:
  void joinRoom(const argus::voice::v1::RtcJoin& join,
                std::function<void(grpc::Status, argus::voice::v1::RtcJoined)> done) override
  {
    rooms.push_back(join.room());
    argus::voice::v1::RtcJoined joined;
    joined.set_joined(true);
    joined.set_already(rooms.size() > 1);
    done(grpc::Status::OK, joined);
  }

  void farewellRoom(const argus::voice::v1::RtcFarewell& farewell, std::function<void()> done) override
  {
    farewells.push_back(farewell.room() + "/" + farewell.reason());
    done();
  }

  std::vector<std::string> rooms;
  std::vector<std::string> farewells;
};

struct UnaryProbe
{
  const std::string& target;
  const std::string& secret;
};

grpc::Status joinWith(const UnaryProbe& input, argus::voice::v1::RtcJoined& joined)
{
  auto stub = argus::voice::v1::VoiceService::NewStub(
      grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, input.secret);
  argus::voice::v1::RtcJoin join;
  join.set_room("u7.rtc-00");
  return stub->JoinRoom(&context, join, &joined);
}

grpc::Status announceWith(const UnaryProbe& input, argus::voice::v1::AnnounceResponse& reply)
{
  auto stub = argus::voice::v1::VoiceService::NewStub(
      grpc::CreateChannel(input.target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, input.secret);
  argus::voice::v1::AnnounceRequest request;
  request.set_user_id(7);
  request.set_text("Ha llegado alguien al patio.");
  return stub->Announce(&context, request, &reply);
}

struct ConnectProbe
{
  const std::string& target;
  const std::string& secret;
};

grpc::Status connectWith(const ConnectProbe& input)
{
  const std::string& target = input.target;
  const std::string& secret = input.secret;
  auto stub = argus::voice::v1::VoiceService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.AddMetadata("x-argus-user", "1");
  context.AddMetadata("x-argus-role", "owner");
  argus::client::addCallerCredential(context, secret);
  auto stream = stub->Connect(&context);
  stream->WritesDone();
  return stream->Finish();
}

}

TEST_CASE("voice answers only the argus-sync caller credential")
{
  VoiceSessionService sessions;
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = nullptr,
                           .dispatchCleanup = {}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(connectWith({.target = target, .secret = ""}).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith({.target = target, .secret = "a-guessed-secret"}).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith({.target = target, .secret = kSyncSecret}).ok());

  server->Shutdown();
}

TEST_CASE("JoinRoom answers only argus-sync, Announce only argus-notification")
{
  VoiceSessionService sessions;
  RecordingJoiner joiner;
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = kNotificationSecret,
                           .rooms = &joiner,
                           .dispatchCleanup = {}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  argus::voice::v1::RtcJoined joined;
  CHECK(joinWith({.target = target, .secret = kNotificationSecret}, joined).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(joinWith({.target = target, .secret = ""}, joined).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  REQUIRE(joinWith({.target = target, .secret = kSyncSecret}, joined).ok());
  CHECK(joined.joined());
  CHECK_FALSE(joined.already());
  CHECK(joiner.rooms == std::vector<std::string>{"u7.rtc-00"});

  {
    auto stub = argus::voice::v1::VoiceService::NewStub(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    argus::voice::v1::RtcFarewell farewell;
    farewell.set_room("u7.rtc-00");
    farewell.set_reason("accountDisabled");
    argus::voice::v1::RtcFarewellDone done;
    grpc::ClientContext refused;
    argus::client::addCallerCredential(refused, kNotificationSecret);
    CHECK(stub->Farewell(&refused, farewell, &done).error_code() == grpc::StatusCode::UNAUTHENTICATED);
    grpc::ClientContext accepted;
    argus::client::addCallerCredential(accepted, kSyncSecret);
    REQUIRE(stub->Farewell(&accepted, farewell, &done).ok());
    CHECK(joiner.farewells == std::vector<std::string>{"u7.rtc-00/accountDisabled"});
  }

  argus::voice::v1::AnnounceResponse reply;
  CHECK(announceWith({.target = target, .secret = kSyncSecret}, reply).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  REQUIRE(announceWith({.target = target, .secret = kNotificationSecret}, reply).ok());
  CHECK_FALSE(reply.delivered());

  server->Shutdown();
}

TEST_CASE("JoinRoom without realtime calls configured is UNAVAILABLE")
{
  VoiceSessionService sessions;
  VoiceRpcService service({.sessions = &sessions,
                           .syncCallerSecret = kSyncSecret,
                           .notificationCallerSecret = "",
                           .rooms = nullptr,
                           .dispatchCleanup = {}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);
  argus::voice::v1::RtcJoined joined;
  CHECK(joinWith({.target = target, .secret = kSyncSecret}, joined).error_code() ==
        grpc::StatusCode::UNAVAILABLE);
  argus::voice::v1::AnnounceResponse reply;
  CHECK(announceWith({.target = target, .secret = ""}, reply).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  server->Shutdown();
}
