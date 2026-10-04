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
  const auto anonymousResult = anonymous.createNotifications(
      request, {.userId = 7, .role = "guest", .device = std::nullopt});
  CHECK(anonymousResult.outcome == NotificationRpcOutcome::Success);
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

namespace
{
class ScriptedCallService final : public v1::CallService::CallbackService
{
public:
  grpc::Status status = grpc::Status::OK;
  v1::ClaimCallRequest claimed;
  v1::EndCallRequest ended;
  v1::ScheduleCallRequest scheduled;
  v1::AnnounceAgendaRequest announced;
  v1::ClaimCallResponse claimAnswer;
  bool endOk = true;
  std::string credential;

  Reactor* ClaimCall(Ctx* context, const v1::ClaimCallRequest* in,
                     v1::ClaimCallResponse* out) override
  {
    claimed = *in;
    *out = claimAnswer;
    return finish(context);
  }
  Reactor* EndCall(Ctx* context, const v1::EndCallRequest* in,
                   v1::EndCallResponse* out) override
  {
    ended = *in;
    out->set_ok(endOk);
    return finish(context);
  }
  Reactor* ScheduleCall(Ctx* context, const v1::ScheduleCallRequest* in,
                        v1::ScheduleCallResponse* out) override
  {
    scheduled = *in;
    out->set_scheduled_id(31);
    out->set_duplicate(true);
    return finish(context);
  }

  Reactor* AnnounceAgenda(Ctx* context, const v1::AnnounceAgendaRequest* in,
                          v1::AnnounceAgendaResponse* out) override
  {
    announced = *in;
    out->set_notified(2);
    out->set_rang(1);
    return finish(context);
  }

private:
  Reactor* finish(Ctx* context)
  {
    credential.clear();
    for (const auto& [key, value] : context->client_metadata())
      if (std::string(key.begin(), key.end()) == "x-argus-credential")
        credential.assign(value.begin(), value.end());
    auto* reactor = context->DefaultReactor();
    reactor->Finish(status);
    return reactor;
  }
};
}

TEST_CASE("the call methods carry their fields and the credential")
{
  ScriptedCallService service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  REQUIRE(server);
  const NotificationClient client(
      {.target = loopback(port), .credential = kCredential});

  service.claimAnswer.set_status(v1::CALL_CLAIM_STATUS_CLAIMED);
  service.claimAnswer.set_opening_line("Hola");
  const auto claim =
      client.claimCall({.callId = "call-4", .userId = 7, .sessionId = "s1"});
  CHECK(claim.outcome == NotificationRpcOutcome::Success);
  CHECK(claim.response.status() == v1::CALL_CLAIM_STATUS_CLAIMED);
  CHECK(claim.response.opening_line() == "Hola");
  CHECK(service.claimed.call_id() == "call-4");
  CHECK(service.claimed.user_id() == 7);
  CHECK(service.claimed.session_id() == "s1");
  CHECK(service.credential == kCredential);

  CHECK(client.endCall({.callId = "call-4",
                        .userId = 7,
                        .outcome = v1::CALL_OUTCOME_DECLINED,
                        .spoken = true}) == NotificationRpcOutcome::Success);
  CHECK(service.ended.outcome() == v1::CALL_OUTCOME_DECLINED);
  CHECK(service.ended.spoken());
  service.endOk = false;
  CHECK(client.endCall({.callId = "call-4",
                        .userId = 7,
                        .outcome = v1::CALL_OUTCOME_COMPLETED,
                        .spoken = false}) == NotificationRpcOutcome::Rejected);

  const auto schedule = client.scheduleCall({.userId = 7,
                                             .fireAt = 1800000000,
                                             .topic = "llamar al dentista",
                                             .lang = "es",
                                             .commandId = "remind-1"});
  CHECK(schedule.outcome == NotificationRpcOutcome::Success);
  CHECK(schedule.scheduledId == 31);
  CHECK(schedule.duplicate);
  CHECK(service.scheduled.topic() == "llamar al dentista");
  CHECK(service.scheduled.fire_at() == 1800000000);

  const auto agenda = client.announceAgenda({.userIds = {7, 8},
                                             .leadMinutes = 15,
                                             .title = "Dentista",
                                             .body = "17:00",
                                             .data = "{}",
                                             .commandId = "agenda:event:1:2:15"});
  CHECK(agenda.outcome == NotificationRpcOutcome::Success);
  CHECK(agenda.notified == 2);
  CHECK(agenda.rang == 1);
  CHECK(service.announced.user_ids_size() == 2);
  CHECK(service.announced.lead_minutes() == 15);
  CHECK(service.announced.command_id() == "agenda:event:1:2:15");

  service.status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");
  CHECK(client.claimCall({.callId = "call-4", .userId = 7, .sessionId = ""})
            .outcome == NotificationRpcOutcome::Unavailable);
  server->Shutdown();
}
