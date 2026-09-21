#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/productivity/v1/sync.grpc.pb.h>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <productivity/productivity-sync-client.hxx>
#include <string>

// Pins the SDK edge: the table branch and its ranges go out untouched, the
// identity rides along, and a refusal answers with no rows at all.
namespace
{
namespace v1 = argus::productivity::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

// Serves whatever the case canned and keeps the request that arrived; a case
// reads the recorded fields once its call returned.
class ScriptedSyncService final : public v1::SyncService::CallbackService
{
public:
  grpc::Status status = grpc::Status::OK;
  v1::PullTableResponse rows;
  v1::PullTableRequest seen;
  std::map<std::string, std::string> metadata;

  Reactor* PullTable(Ctx* context, const v1::PullTableRequest* request,
                     v1::PullTableResponse* out) override
  {
    seen = *request;
    metadata.clear();
    for (const auto& [key, value] : context->client_metadata())
      metadata.emplace(std::string(key.begin(), key.end()),
                       std::string(value.begin(), value.end()));
    *out = rows;
    auto* reactor = context->DefaultReactor();
    reactor->Finish(status);
    return reactor;
  }
};

std::unique_ptr<grpc::Server> startServer(ScriptedSyncService& service,
                                          int& port)
{
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  return std::unique_ptr<grpc::Server>(builder.BuildAndStart());
}

std::string loopback(int port)
{
  return "127.0.0.1:" + std::to_string(port);
}

argus::client::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "owner", .device = "abc123"};
}
} // namespace

TEST_CASE("a pull sends the requested branch and the identity untouched")
{
  ScriptedSyncService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  const ProductivitySyncClient client(loopback(port));

  auto* task = service.rows.mutable_project_task();
  task->add_created()->set_title("Ship the release");
  task->mutable_created(0)->set_sort_order(1.5);
  task->add_deleted()->set_deleted_at(900);
  task->mutable_last_created()->set_id(88);

  v1::PullTableRequest request;
  request.mutable_project_task()->set_required_create(true);
  request.mutable_project_task()->mutable_created()->set_start_id(40);
  const auto rows = client.pullTable(request, identityFor(7));
  REQUIRE(rows.has_value());
  CHECK(service.seen.table_case() == v1::PullTableRequest::kProjectTask);
  CHECK(service.seen.project_task().required_create());
  CHECK(service.seen.project_task().created().start_id() == 40);
  CHECK(service.metadata.at("x-argus-user") == "7");
  CHECK(service.metadata.at("x-argus-role") == "owner");
  CHECK(service.metadata.at("x-argus-device") == "abc123");
  CHECK(service.metadata.count("x-argus-credential") == 0);
  CHECK(rows->project_task().created(0).title() == "Ship the release");
  CHECK(rows->project_task().created(0).sort_order() == doctest::Approx(1.5));
  CHECK(rows->project_task().deleted(0).deleted_at() == 900);
  CHECK(rows->project_task().last_created().id() == 88);

  // Another request pulls another table: the branch is the caller's, and the
  // client adds nothing of its own to it.
  service.rows.clear_project_task();
  service.rows.mutable_reminder()->add_created()->set_id(3);
  v1::PullTableRequest other;
  other.mutable_reminder()->set_required_deleted(true);
  const auto later = client.pullTable(other, identityFor(7));
  REQUIRE(later.has_value());
  CHECK(service.seen.table_case() == v1::PullTableRequest::kReminder);
  CHECK(service.seen.reminder().required_deleted());
  CHECK(later->reminder().created(0).id() == 3);

  server->Shutdown();
}

TEST_CASE("a refusal and an unreachable receiver both answer no rows")
{
  ScriptedSyncService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  const ProductivitySyncClient client(loopback(port));
  service.rows.mutable_reminder()->add_created()->set_id(3);

  v1::PullTableRequest request;
  request.mutable_reminder()->set_required_deleted(true);

  service.status = grpc::Status(grpc::StatusCode::NOT_FOUND, "no account");
  CHECK_FALSE(client.pullTable(request, identityFor(7)).has_value());
  service.status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "no");
  CHECK_FALSE(client.pullTable(request, identityFor(7)).has_value());
  service.status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");
  CHECK_FALSE(client.pullTable(request, identityFor(7)).has_value());

  // The same body with an OK status does come back, so the empty results above
  // are the status's doing and not rows the client dropped.
  service.status = grpc::Status::OK;
  const auto rows = client.pullTable(request, identityFor(7));
  REQUIRE(rows.has_value());
  CHECK(rows->reminder().created(0).id() == 3);

  const ProductivitySyncClient dead("127.0.0.1:1");
  CHECK_FALSE(dead.pullTable(request, identityFor(7)).has_value());

  server->Shutdown();
}
