#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/sync/camera-sync-rpc-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

namespace
{

constexpr const char* kSyncSecret = "camera-sync-auth-test-secret";

struct PullProbe
{
  const std::string& target;
  const std::string& secret;
};

grpc::StatusCode pullWith(const PullProbe& input)
{
  const std::string& target = input.target;
  const std::string& secret = input.secret;
  auto stub = argus::camera::v1::SyncService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(500));
  argus::client::addCallerIdentity(
      context, {.userId = 1, .role = "owner", .device = "test"});
  argus::client::addCallerCredential(context, secret);
  argus::camera::v1::PullTableResponse response;
  return stub->PullTable(&context, {}, &response).error_code();
}

}

TEST_CASE("the camera sync leg answers only a credentialed caller")
{
  CameraSyncRpcService service(
      {argus::client::CallerCredential{.service = "argus-sync",
                                       .secret = kSyncSecret}});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(pullWith({.target = target, .secret = ""}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(pullWith({.target = target, .secret = "guessed"}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(pullWith({.target = target, .secret = kSyncSecret}) == grpc::StatusCode::INVALID_ARGUMENT);

  server->Shutdown(std::chrono::system_clock::now());
}
