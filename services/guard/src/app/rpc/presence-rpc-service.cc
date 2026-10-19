#include "presence-rpc-service.hxx"

#include <drogon/drogon.h>
#include <feature/presence/services/presence-service.hxx>
#include <trantor/utils/Logger.h>

#include <optional>
#include <string>
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

grpc::ServerUnaryReactor* PresenceRpcService::GetDuty(
    grpc::CallbackServerContext* context,
    const v1::DutyRequest* request, v1::DutyResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, input_.credentials)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Caller credential missing or invalid"));
    return reactor;
  }
  if (!input_.duty) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "Duty is not wired"));
    return reactor;
  }
  const int64_t userId = request->user_id();
  DutyLookup duty = input_.duty;
  drogon::app().getLoop()->queueInLoop([duty = std::move(duty), reactor, response, userId]() {
    drogon::async_run([duty, reactor, response, userId]() -> drogon::Task<void> {
      try {
        const std::vector<int64_t> environments = co_await duty(userId);
        response->set_on_duty(!environments.empty());
        for (const int64_t environment : environments)
          response->add_environment_ids(environment);
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Presence RPC: GetDuty failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Duty failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* PresenceRpcService::CameraRooms(
    grpc::CallbackServerContext* context,
    const v1::CameraRoomsRequest*, v1::CameraRoomsResponse* response)
{
  auto* reactor = context->DefaultReactor();
  if (!argus::client::authorizeCaller(context, input_.credentials)) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Caller credential missing or invalid"));
    return reactor;
  }
  if (!input_.rooms) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "Rooms are not wired"));
    return reactor;
  }
  CameraRoomLookup rooms = input_.rooms;
  drogon::app().getLoop()->queueInLoop([rooms = std::move(rooms), reactor, response]() {
    drogon::async_run([rooms, reactor, response]() -> drogon::Task<void> {
      try {
        for (const CameraRoomRow& row : co_await rooms()) {
          auto* camera = response->add_cameras();
          camera->set_camera_id(row.cameraId);
          camera->set_environment_id(row.environmentId);
          camera->set_room(row.room);
          camera->set_outdoor(row.outdoor);
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Presence RPC: CameraRooms failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Rooms failed"));
      }
    });
  });
  return reactor;
}

grpc::ServerUnaryReactor* PresenceRpcService::ReportVoiceEvidence(
    grpc::CallbackServerContext* context, const v1::VoiceEvidenceRequest* request,
    v1::VoiceEvidenceResponse* response)
{
  auto* reactor = context->DefaultReactor();
  const std::optional<std::string> caller = argus::client::authorizeCaller(context, input_.credentials);
  if (!caller || *caller != kVoiceEvidenceCaller) {
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "Caller credential missing or invalid"));
    return reactor;
  }
  if (!input_.voice) {
    response->set_accepted(false);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
  const VoiceEvidenceRow row{.userId = request->user_id(),
                             .cameraId = request->camera_id(),
                             .snrDb = request->snr_db(),
                             .voiceprint = request->voiceprint(),
                             .atMs = request->at_ms()};
  VoiceEvidenceSink voice = input_.voice;
  drogon::app().getLoop()->queueInLoop([voice = std::move(voice), reactor, response, row]() {
    drogon::async_run([voice, reactor, response, row]() -> drogon::Task<void> {
      try {
        response->set_accepted(co_await voice(row));
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& error) {
        LOG_WARN << "Presence RPC: ReportVoiceEvidence failed: " << error.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Voice evidence failed"));
      }
    });
  });
  return reactor;
}
