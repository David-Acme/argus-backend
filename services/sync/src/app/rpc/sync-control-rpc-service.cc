#include "sync-control-rpc-service.hxx"
#include "sync-control-callers.hxx"

#include <auth/user-role.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <string_view>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <utility>

namespace
{
SocketEmitDto fromFrame(const argus::sync::v1::SyncFrame& frame)
{
  SocketEmitDto dto;
  dto.operation = static_cast<SyncOperation>(frame.operation());
  dto.option = static_cast<TableName>(frame.table());
  dto.obj = json_util::fromString(frame.info());
  return dto;
}

bool isRoleName(const std::string& name)
{
  return userRoleToString(userRoleFromString(name)) == name;
}

bool dispatchPayload(const Json::Value& payload)
{
  const auto event = sync_fan_out::parseEvent(payload);
  if (!event)
    return false;
  sync_fan_out::dispatchEvent(*event);
  return true;
}

struct AckInput
{
  argus::sync::v1::ControlAck* response{nullptr};
  bool ok{false};
  std::string_view reason;
};

grpc::ServerUnaryReactor* finishAck(grpc::CallbackServerContext* context,
                                    const AckInput& input)
{
  input.response->set_ok(input.ok);
  input.response->set_reason(std::string(input.reason));
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* reject(grpc::CallbackServerContext* context,
                                 argus::client::FleetVerdict verdict)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(argus::client::FleetCallerGate::refusal(verdict));
  return reactor;
}

bool callFrame(const argus::sync::v1::SyncFrame& frame)
{
  const auto operation = static_cast<SyncOperation>(frame.operation());
  return operation == SyncOperation::CallIncoming ||
         operation == SyncOperation::CallCancel ||
         operation == SyncOperation::ResponseUpdate;
}
}

SyncControlRpcService::SyncControlRpcService(
    std::shared_ptr<const argus::client::FleetCallerGate> gate)
    : gate_(std::move(gate))
{
}

argus::client::FleetAdmission
SyncControlRpcService::admit(const grpc::CallbackServerContext* context,
                             argus::client::CallerSet allowed) const
{
  return gate_->admit(context, allowed);
}

grpc::ServerUnaryReactor* SyncControlRpcService::ReplaceRoleRooms(
    grpc::CallbackServerContext* context,
    const argus::sync::v1::ReplaceRoleRoomsRequest* request,
    argus::sync::v1::ControlAck* response)
{
  if (const auto admission = admit(context, sync_control_callers::kRoomControl);
      !admission.admitted())
    return reject(context, admission.verdict);

  if (request->user_id() <= 0 || !isRoleName(request->old_role()) ||
      !isRoleName(request->new_role()))
    return finishAck(context, {.response = response,
                               .ok = false,
                               .reason = "unknown role"});

  const bool ok = dispatchPayload(
      sync_change::roleRoomsPayload({.userId = request->user_id(),
                                     .oldRole = request->old_role(),
                                     .newRole = request->new_role()}));
  return finishAck(context, {.response = response,
                             .ok = ok,
                             .reason = ok ? "" : "unroutable frame"});
}

grpc::ServerUnaryReactor* SyncControlRpcService::DisconnectUser(
    grpc::CallbackServerContext* context,
    const argus::sync::v1::DisconnectUserRequest* request,
    argus::sync::v1::ControlAck* response)
{
  if (const auto admission = admit(context, sync_control_callers::kRoomControl);
      !admission.admitted())
    return reject(context, admission.verdict);

  if (request->user_id() <= 0 || !request->has_frame())
    return finishAck(context, {.response = response,
                               .ok = false,
                               .reason = "unroutable frame"});

  const bool ok = dispatchPayload(
      sync_change::disconnectPayload(fromFrame(request->frame()),
                                     request->user_id()));
  return finishAck(context, {.response = response,
                             .ok = ok,
                             .reason = ok ? "" : "unroutable frame"});
}

grpc::ServerUnaryReactor* SyncControlRpcService::EmitToUser(
    grpc::CallbackServerContext* context,
    const argus::sync::v1::EmitToUserRequest* request,
    argus::sync::v1::ControlAck* response)
{
  if (const auto admission = admit(context, sync_control_callers::kEmitToUser);
      !admission.admitted())
    return reject(context, admission.verdict);

  if (!callFrame(request->frame())) {
    const auto admission = admit(context, sync_control_callers::kRoomControl);
    if (!admission.admitted())
      return reject(context, admission.verdict);
  }

  if (request->user_id() <= 0 || !request->has_frame())
    return finishAck(context, {.response = response,
                               .ok = false,
                               .reason = "unroutable frame"});

  const bool ok =
      dispatchPayload(sync_change::userEmitPayload(fromFrame(request->frame()),
                                                   {request->user_id()}));
  return finishAck(context, {.response = response,
                             .ok = ok,
                             .reason = ok ? "" : "unroutable frame"});
}
