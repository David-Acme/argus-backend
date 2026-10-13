#include "duress-rpc-service.hxx"

#include <auth/capability.hxx>
#include <auth/module-gate.hxx>
#include <auth/user-role.hxx>
#include <drogon/drogon.h>
#include <feature/safety/infra/safety-ports.hxx>
#include <feature/safety/services/safety-service.hxx>
#include <trantor/utils/Logger.h>

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
      {.userId = request.userId, .userName = actor.name, .phraseKey = request.key}));
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
       call = SpokenRequest{.userId = request->user_id(), .role = request->role(), .key = request->key()},
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
