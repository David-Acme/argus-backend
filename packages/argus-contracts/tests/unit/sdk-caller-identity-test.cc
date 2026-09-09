#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <argus/camera/v1/sync.grpc.pb.h>
#include <camera/camera-sync-client.hxx>
#include <grpcpp/grpcpp.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

// Receivers gate on the PRESENCE of the x-argus-* keys, not their values:
// argus-camera's sync service rejects a request missing any of the three,
// and in credential mode the device hash is legitimately empty. This suite
// pins that contract against the shared SDK client base.
namespace
{

class CapturingSyncService final
    : public argus::camera::v1::SyncService::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::camera::v1::PullTableRequest*,
            argus::camera::v1::PullTableResponse* response) override
  {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      seen_.clear();
      for (const auto& [key, value] : context->client_metadata())
        seen_.emplace(std::string(key.begin(), key.end()),
                      std::string(value.begin(), value.end()));
    }
    response->mutable_camera();
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  std::map<std::string, std::string> metadata() const
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> seen_;
};

} // namespace

TEST_CASE("caller identity metadata travels by presence, not by value")
{
  CapturingSyncService service;
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);

  CameraSyncClient client("127.0.0.1:" + std::to_string(port));
  argus::camera::v1::PullTableRequest request;
  request.mutable_camera()->set_required_create(true);

  SUBCASE("a populated identity sends all three keys")
  {
    const auto response = client.pullTable(
        request, {.userId = 7, .role = "owner", .device = "abc123"});
    REQUIRE(response);
    const auto seen = service.metadata();
    CHECK(seen.at("x-argus-user") == "7");
    CHECK(seen.at("x-argus-role") == "owner");
    CHECK(seen.at("x-argus-device") == "abc123");
  }

  SUBCASE("an empty device hash is still sent as a present, empty header")
  {
    const auto response =
        client.pullTable(request, {.userId = 7, .role = "guest", .device = ""});
    REQUIRE(response);
    const auto seen = service.metadata();
    CHECK(seen.count("x-argus-user") == 1);
    CHECK(seen.count("x-argus-role") == 1);
    CHECK(seen.count("x-argus-device") == 1);
    CHECK(seen.at("x-argus-device").empty());
  }

  server->Shutdown();
}
