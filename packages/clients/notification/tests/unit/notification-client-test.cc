#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/notification/v1/notification.grpc.pb.h>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <notification/notification-client.hxx>
#include <optional>
#include <string>

namespace
{
constexpr const char* kCredential = "guard-notif-cred";
namespace v1 = argus::notification::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ScriptedNotificationService final
    : public v1::NotificationService::CallbackService
{
public:
  grpc::Status status = grpc::Status::OK;
  v1::CreateNotificationsResponse created;
  v1::PullNotificationsResponse pulled;
  std::map<std::string, std::string> metadata;
  Reactor* CreateNotifications(Ctx* context,
                               const v1::CreateNotificationsRequest*,
                               v1::CreateNotificationsResponse* out) override
  {
    *out = created;
    return finish(context);
  }
  Reactor* PullNotifications(Ctx* context, const v1::PullNotificationsRequest*,
                             v1::PullNotificationsResponse* out) override
  {
    *out = pulled;
    return finish(context);
  }

private:
  Reactor* finish(Ctx* context)
  {
    metadata.clear();
    for (const auto& [key, value] : context->client_metadata())
      metadata.emplace(std::string(key.begin(), key.end()),
                       std::string(value.begin(), value.end()));
    auto* reactor = context->DefaultReactor();
    reactor->Finish(status);
    return reactor;
  }
};

std::unique_ptr<grpc::Server> startServer(ScriptedNotificationService& service,
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

v1::CreateNotificationsRequest createRequest()
{
  v1::CreateNotificationsRequest request;
  request.add_user_ids(7);
  request.set_type("camera");
  request.set_title("Front door: person");
  request.set_command_id("cmd-1");
  return request;
}

argus::client::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "owner", .device = "abc123"};
}
}

TEST_CASE("a create carries the identity and the capability credential")
{
  ScriptedNotificationService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  service.created.set_created(2);
  const auto request = createRequest();

  const NotificationClient configured(
      {.target = loopback(port), .credential = kCredential});
  const auto result = configured.createNotifications(request, identityFor(7));
  CHECK(result.outcome == NotificationRpcOutcome::Success);
  CHECK(result.created == 2);
  CHECK(service.metadata.at("x-argus-user") == "7");
  CHECK(service.metadata.at("x-argus-role") == "owner");
  CHECK(service.metadata.at("x-argus-device") == "abc123");
  CHECK(service.metadata.at("x-argus-credential") == kCredential);

  const NotificationClient anonymous(
      {.target = loopback(port), .credential = ""});
  anonymous.createNotifications(request, {.userId = 7,
                                          .role = "guest",
                                          .device = std::nullopt});
  CHECK(service.metadata.count("x-argus-credential") == 0);
  CHECK(service.metadata.count("x-argus-device") == 0);
  server->Shutdown();
}

TEST_CASE("a receiver status picks the outcome, and only OK carries a body")
{
  ScriptedNotificationService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  const NotificationClient client(
      {.target = loopback(port), .credential = kCredential});
  const auto request = createRequest();
  service.status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "seen");
  service.created.set_created(9);
  service.created.set_duplicate(true);
  const auto conflict = client.createNotifications(request, identityFor(0));
  CHECK(conflict.outcome == NotificationRpcOutcome::Conflict);
  CHECK(conflict.status.error_code() == grpc::StatusCode::ALREADY_EXISTS);
  CHECK(conflict.created == 0);
  CHECK_FALSE(conflict.duplicate);

  service.status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");
  CHECK(client.createNotifications(request, identityFor(0)).outcome ==
        NotificationRpcOutcome::Unavailable);
  service.status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "who");
  CHECK(client.createNotifications(request, identityFor(0)).outcome ==
        NotificationRpcOutcome::Rejected);

  const NotificationClient dead({.target = "127.0.0.1:1", .credential = ""});
  CHECK(dead.createNotifications(request, identityFor(0)).outcome ==
        NotificationRpcOutcome::Unavailable);

  service.status = grpc::Status::OK;
  service.pulled.add_created()->set_id(11);
  service.pulled.mutable_created(0)->set_title("Front door: person");
  service.pulled.mutable_last_created()->set_id(12);
  const auto result = client.pullNotifications({}, identityFor(7));
  CHECK(result.outcome == NotificationRpcOutcome::Success);
  REQUIRE(result.response.created_size() == 1);
  CHECK(result.response.created(0).title() == "Front door: person");
  CHECK(result.response.last_created().id() == 12);

  server->Shutdown();
}
