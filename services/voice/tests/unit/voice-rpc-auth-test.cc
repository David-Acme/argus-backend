#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/voice-rpc-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace
{

constexpr const char* kSyncSecret = "voice-rpc-auth-test-secret";

grpc::Status connectWith(const std::string& target, const std::string& secret)
{
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
  VoiceRpcService service(kSyncSecret);
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(connectWith(target, "").error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith(target, "a-guessed-secret").error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith(target, kSyncSecret).ok());

  server->Shutdown();
}
