#include "duress-rpc-service.hxx"

#include <auth/capability.hxx>
#include <auth/module-gate.hxx>
#include <auth/user-role.hxx>
#include <drogon/drogon.h>
#include <feature/safety/infra/safety-ports.hxx>
#include <feature/safety/services/safety-service.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace
{
namespace v1 = argus::guard::v1;

v1::SpokenDuressOutcome wireOutcome(SpokenDuressOutcome outcome)
{
  switch (outcome) {
    case SpokenDuressOutcome::Raised:
      return v1::SPOKEN_DURESS_OUTCOME_RAISED;
    case SpokenDuressOutcome::Repeated:
      return v1::SPOKEN_DURESS_OUTCOME_REPEATED;
    case SpokenDuressOutcome::Stale:
      return v1::SPOKEN_DURESS_OUTCOME_STALE;
  }
  return v1::SPOKEN_DURESS_OUTCOME_UNSPECIFIED;
}

struct SpokenRequest
{
  int64_t userId{0};
  std::string role;
  std::string key;
  int64_t saidAt{0};
};

struct SpokenContext
{
  std::shared_ptr<const SafetyService> safety;
  const SafetyActorDirectory* actors{nullptr};
};

drogon::Task<v1::SpokenDuressOutcome> raise(const SpokenContext& context, const SpokenRequest& request)
{
  SafetyActor actor;
  if (context.actors != nullptr)
    actor = co_await context.actors->actorOf(request.userId);
  if (actor.known && !actor.active)
    co_return v1::SPOKEN_DURESS_OUTCOME_NOT_ALLOWED;
  const UserRole role = userRoleFromString(actor.known && !actor.role.empty() ? actor.role : request.role);
  if (!role_access::hasCapability(
          {.role = role, .modules = moduleGate().snapshot(), .capability = "safety.duress"}))
    co_return v1::SPOKEN_DURESS_OUTCOME_NOT_ALLOWED;
  co_return wireOutcome(co_await context.safety->spokenDuress(
      {.userId = request.userId, .userName = actor.name, .phraseKey = request.key, .saidAt = request.saidAt}));
}

grpc::ServerUnaryReactor* refuse(grpc::CallbackServerContext* context, const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}
}

DuressRpcService::DuressRpcService(DuressRpcInput input) : input_(std::move(input)) {}

grpc::ServerUnaryReactor* DuressRpcService::GetPhrase(grpc::CallbackServerContext* context,
                                                      const v1::DuressPhraseRequest*,
                                                      v1::DuressPhraseResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Caller credential missing or invalid"));
  if (!input_.safety)
    return refuse(context, grpc::Status(grpc::StatusCode::UNAVAILABLE, "Safety is not wired"));
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([safety = input_.safety, reactor, response]() {
    drogon::async_run([safety, reactor, response]() -> drogon::Task<void> {
      try {
        response->set_key(co_await safety->phraseKey());
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Duress RPC: GetPhrase failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Safety failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* DuressRpcService::RaiseSpoken(grpc::CallbackServerContext* context,
                                                        const v1::SpokenDuressRequest* request,
                                                        v1::SpokenDuressResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.credentials))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Caller credential missing or invalid"));
  if (!input_.safety)
    return refuse(context, grpc::Status(grpc::StatusCode::UNAVAILABLE, "Safety is not wired"));
  if (request->user_id() <= 0 || request->key().empty())
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "A speaker and a key are required"));
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop(
      [spoken = SpokenContext{.safety = input_.safety, .actors = input_.actors},
       call = SpokenRequest{.userId = request->user_id(),
                            .role = request->role(),
                            .key = request->key(),
                            .saidAt = request->said_at()},
       reactor,
       response]() {
        drogon::async_run([spoken, call, reactor, response]() -> drogon::Task<void> {
          try {
            response->set_outcome(co_await raise(spoken, call));
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& error) {
            LOG_WARN << "Duress RPC: an alert could not be recorded: " << error.what();
            reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Safety failed"));
          }
        });
      });
  return reactor;
}

grpc::ServerUnaryReactor* DuressRpcService::RaiseCameraPanic(grpc::CallbackServerContext* context,
                                                             const v1::CameraPanicRequest* request,
                                                             v1::CameraPanicResponse* response)
{
  if (!argus::client::authorizeCaller(context, input_.panicCredentials))
    return refuse(context, grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Caller credential missing or invalid"));
  if (!input_.safety)
    return refuse(context, grpc::Status(grpc::StatusCode::UNAVAILABLE, "Safety is not wired"));
  if (request->camera_id() <= 0)
    return refuse(context, grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "A camera is required"));
  auto* reactor = context->DefaultReactor();
  drogon::app().getLoop()->queueInLoop([safety = input_.safety,
                                        environmentOf = input_.environmentOf,
                                        cameraId = request->camera_id(),
                                        userId = std::max<int64_t>(0, request->user_id()),
                                        speaker = request->speaker(),
                                        saidAt = request->said_at(),
                                        duress = request->duress(),
                                        reactor,
                                        response]() {
    drogon::async_run([safety, environmentOf, cameraId, userId, speaker, saidAt, duress, reactor,
                       response]() -> drogon::Task<void> {
      try {
        int64_t environment = 0;
        if (environmentOf)
          environment = co_await environmentOf(cameraId);
        const std::optional<int64_t> environmentId =
            environment > 0 ? std::optional<int64_t>(environment) : std::nullopt;
        PanicResult result;
        if (duress && userId == 0)
          result = co_await safety->cameraDuress({.speaker = speaker, .environmentId = environmentId, .saidAt = saidAt});
        else
          result = co_await safety->panic({.userId = userId, .userName = speaker, .environmentId = environmentId});
        response->set_sent(result.sent);
        response->set_repeated(result.repeated);
        response->set_alert_id(result.alertId);
        LOG_WARN << "Duress RPC: " << (duress ? "an alert" : "panic") << " raised through camera " << cameraId
                 << " (alert " << result.alertId << ")";
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Duress RPC: a camera panic could not be recorded: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Safety failed"));
      }
    });
  });
  return reactor;
}
