#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/camera/v1/sync.grpc.pb.h>
#include <atomic>
#include <camera/camera-sync-client.hxx>
#include <chrono>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace
{

namespace v1 = argus::camera::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ScriptedSyncService final : public v1::SyncService::CallbackService
{
public:
  grpc::Status status = grpc::Status::OK;
  std::atomic<bool> zoneBranch{false};
  std::atomic<int64_t> startId{0};
  std::atomic<int64_t> catalogBytes{-1};
  std::atomic<int64_t> deadlineMs{0};

  Reactor* PullTable(Ctx* context, const v1::PullTableRequest* request,
                     v1::PullTableResponse* out) override
  {
    zoneBranch = request->has_zone();
    startId = request->camera().created().start_id();
    const auto ms = context->deadline() - std::chrono::system_clock::now();
    deadlineMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(ms).count();
    out->mutable_camera()->add_created()->set_name("porton");
    return answer(context);
  }

  Reactor* ListCatalog(Ctx* context, const v1::ListCatalogRequest* request,
                       v1::ListCatalogResponse* out) override
  {
    catalogBytes = static_cast<int64_t>(request->ByteSizeLong());
    const auto ms = context->deadline() - std::chrono::system_clock::now();
    deadlineMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(ms).count();
    out->add_cameras()->set_name("porton");
    return answer(context);
  }

private:
  Reactor* answer(Ctx* context)
  {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(status);
    return reactor;
  }
};

std::unique_ptr<grpc::Server> startServer(ScriptedSyncService& service,
                                          std::string& target)
{
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = std::unique_ptr<grpc::Server>(builder.BuildAndStart());
  target = "127.0.0.1:" + std::to_string(port);
  return server;
}

}

TEST_CASE("a pull carries the caller's branch and cursor onto the wire")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const CameraSyncClient client({.target = target, .credential = ""});

  v1::PullTableRequest request;
  request.mutable_camera()->mutable_created()->set_start_id(41);
  const auto answer =
      client.pullTable(request,
                       {.userId = 7, .role = "owner", .device = "abc123"});
  REQUIRE(answer);
  CHECK(answer->camera().created(0).name() == "porton");
  CHECK_FALSE(answer->has_camera_stream());
  CHECK(service.startId == 41);
  CHECK_FALSE(service.zoneBranch);

  server->Shutdown();
}

TEST_CASE("the catalog read leaves everything but the identity at home")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const CameraSyncClient client({.target = target, .credential = ""});

  const auto catalog =
      client.listCatalog({.userId = 7, .role = "owner", .device = "abc123"});
  REQUIRE(catalog);
  CHECK(catalog->cameras(0).name() == "porton");
  CHECK(service.catalogBytes == 0);

  server->Shutdown();
}

TEST_CASE("every call carries a deadline, and a refusal is not an empty table")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const CameraSyncClient client({.target = target, .credential = ""});
  const SyncIdentity identity{.userId = 7, .role = "owner", .device = "abc123"};

  v1::PullTableRequest request;
  request.mutable_zone()->set_find_last_deleted(true);
  REQUIRE(client.pullTable(request, identity));
  CHECK(service.zoneBranch);
  CHECK_MESSAGE(service.deadlineMs.load() >= 4000,
                "pull deadlineMs=" << service.deadlineMs.load());
  CHECK_MESSAGE(service.deadlineMs.load() <= 6000,
                "pull deadlineMs=" << service.deadlineMs.load());

  REQUIRE(client.listCatalog(identity));
  CHECK_MESSAGE(service.deadlineMs.load() >= 4000,
                "list deadlineMs=" << service.deadlineMs.load());
  CHECK_MESSAGE(service.deadlineMs.load() <= 6000,
                "list deadlineMs=" << service.deadlineMs.load());

  service.status = grpc::Status(grpc::StatusCode::NOT_FOUND, "no zone");
  CHECK_FALSE(client.pullTable(request, identity).has_value());
  CHECK_FALSE(
      CameraSyncClient({.target = "127.0.0.1:1", .credential = ""}).listCatalog(identity).has_value());

  server->Shutdown();
}
