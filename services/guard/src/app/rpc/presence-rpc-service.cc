#include "presence-rpc-service.hxx"

#include <drogon/drogon.h>
#include <feature/presence/services/presence-service.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
namespace v1 = argus::guard::v1;

v1::PresenceState wireState(PresenceState state)
{
  switch (state) {
    case PresenceState::Home:
      return v1::PRESENCE_STATE_HOME;
    case PresenceState::Away:
      return v1::PRESENCE_STATE_AWAY;
    case PresenceState::Unknown:
      return v1::PRESENCE_STATE_UNKNOWN;
  }
  return v1::PRESENCE_STATE_UNKNOWN;
}

v1::PresenceSource wireSource(PresenceSource source)
{
  switch (source) {
    case PresenceSource::LanSession:
      return v1::PRESENCE_SOURCE_LAN_SESSION;
    case PresenceSource::TunnelSession:
      return v1::PRESENCE_SOURCE_TUNNEL_SESSION;
    case PresenceSource::AppActivity:
      return v1::PRESENCE_SOURCE_APP_ACTIVITY;
    case PresenceSource::Camera:
      return v1::PRESENCE_SOURCE_CAMERA;
    case PresenceSource::Timeout:
      return v1::PRESENCE_SOURCE_TIMEOUT;
    case PresenceSource::Consent:
    case PresenceSource::None:
      return v1::PRESENCE_SOURCE_NONE;
  }
  return v1::PRESENCE_SOURCE_NONE;
}

void fill(const std::vector<PresenceUserView>& views,
          v1::ListPresenceResponse& response)
{
  for (const PresenceUserView& view : views) {
    auto* user = response.add_users();
    user->set_user_id(view.userId);
    user->set_overall(wireState(view.overall));
    user->set_since(view.since);
    for (const PresenceRow& row : view.environments) {
      auto* environment = user->add_environments();
      environment->set_environment_id(row.environmentId);
      environment->set_state(wireState(row.state));
      environment->set_source(wireSource(row.source));
      environment->set_since(row.since);
    }
  }
}
}

PresenceRpcService::PresenceRpcService(PresenceRpcInput input)
    : input_(std::move(input))
{
}

grpc::ServerUnaryReactor* PresenceRpcService::ListPresence(
    grpc::CallbackServerContext* context,
    const v1::ListPresenceRequest* request, v1::ListPresenceResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, input_.credentials)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Caller credential missing or invalid"));
    return reactor;
  }
  if (input_.presence == nullptr) {
    reactor->Finish(
        grpc::Status(grpc::StatusCode::UNAVAILABLE, "Presence is not wired"));
    return reactor;
  }
  std::vector<int64_t> userIds(request->user_ids().begin(),
                               request->user_ids().end());
  const PresenceService* presence = input_.presence;
  drogon::app().getLoop()->queueInLoop(
      [presence, reactor, response, userIds = std::move(userIds)]() mutable {
        drogon::async_run([presence, reactor, response,
                           userIds = std::move(userIds)]() -> drogon::Task<void> {
          try {
            fill(co_await presence->snapshot(userIds), *response);
            reactor->Finish(grpc::Status::OK);
          }
          catch (const std::exception& error) {
            LOG_WARN << "Presence RPC: ListPresence failed: " << error.what();
            reactor->Finish(
                grpc::Status(grpc::StatusCode::INTERNAL, "Presence failed"));
          }
        });
      });
  return reactor;
}
