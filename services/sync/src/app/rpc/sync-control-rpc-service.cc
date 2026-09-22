#include "sync-control-rpc-service.hxx"

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
// The wire carries the frame typed -- both enums as the numbers their C++
// counterparts carry, the row as JSON text -- while the fan-out reads the
// envelope's own spelling: option as the table's name, info as the object.
SocketEmitDto fromFrame(const argus::sync::v1::SyncFrame& frame)
{
  SocketEmitDto dto;
  dto.operation = static_cast<SyncOperation>(frame.operation());
  dto.option = static_cast<TableName>(frame.table());
  dto.obj = json_util::fromString(frame.info());
  return dto;
}

// A role name that does not survive the round trip through the enum is not a
// role: refusing beats letting the parser's fallback silently change the
// caller's rooms.
bool isRoleName(const std::string& name)
{
  return userRoleToString(userRoleFromString(name)) == name;
}

// Both legs end here: the payload the change feed would have carried is parsed
// and dispatched exactly as a NATS frame is.
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

grpc::ServerUnaryReactor*
rejectUnauthenticated(grpc::CallbackServerContext* context)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                               "Fleet secret missing or invalid"));
  return reactor;
}
} // namespace

SyncControlRpcService::SyncControlRpcService(std::string fleetSecret)
    : fleetSecret_(std::move(fleetSecret))
{
}

bool SyncControlRpcService::fleetAuthorized(
    const grpc::CallbackServerContext* context) const
{
  if (fleetSecret_.empty())
    return true;
  return argus::client::constantTimeEquals(
      argus::client::metadata(context, argus::client::kFleetSecretKey),
      fleetSecret_);
}

grpc::ServerUnaryReactor* SyncControlRpcService::ReplaceRoleRooms(
    grpc::CallbackServerContext* context,
    const argus::sync::v1::ReplaceRoleRoomsRequest* request,
    argus::sync::v1::ControlAck* response)
{
  if (!fleetAuthorized(context))
    return rejectUnauthenticated(context);

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
  if (!fleetAuthorized(context))
    return rejectUnauthenticated(context);

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
  if (!fleetAuthorized(context))
    return rejectUnauthenticated(context);

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
