#include "sync-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <text/json-util.hxx>

namespace
{
constexpr int kCallTimeoutMs = 5000;

argus::sync::v1::SyncFrame toFrame(const SocketEmitDto& dto)
{
  argus::sync::v1::SyncFrame frame;
  frame.set_operation(
      static_cast<argus::sync::v1::SyncOperation>(dto.operation));
  frame.set_table(static_cast<argus::sync::v1::TableName>(dto.option));
  frame.set_info(json_util::toString(dto.obj));
  return frame;
}

bool answer(const grpc::Status& status, const argus::sync::v1::ControlAck& ack)
{
  return status.ok() && ack.ok();
}
}

SyncClient::SyncClient(SyncClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::sync::v1::SyncControlService::NewStub(channel_)),
      credential_({.credential = std::move(config.credential),
                   .fleetSecret = std::move(config.fleetSecret)})
{
}

bool SyncClient::replaceRoleRooms(
    const sync_change::RoleRoomChange& change) const
{
  if (change.userId <= 0)
    return false;

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addPeerCredential(context, credential_);

  argus::sync::v1::ReplaceRoleRoomsRequest request;
  request.set_user_id(change.userId);
  request.set_old_role(change.oldRole);
  request.set_new_role(change.newRole);

  argus::sync::v1::ControlAck ack;
  return answer(stub_->ReplaceRoleRooms(&context, request, &ack), ack);
}

bool SyncClient::disconnectUser(int64_t userId,
                                const SocketEmitDto& frame) const
{
  if (userId <= 0)
    return false;

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addPeerCredential(context, credential_);

  argus::sync::v1::DisconnectUserRequest request;
  request.set_user_id(userId);
  *request.mutable_frame() = toFrame(frame);

  argus::sync::v1::ControlAck ack;
  return answer(stub_->DisconnectUser(&context, request, &ack), ack);
}

bool SyncClient::emitToUser(int64_t userId, const SocketEmitDto& frame) const
{
  if (userId <= 0)
    return false;

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addPeerCredential(context, credential_);

  argus::sync::v1::EmitToUserRequest request;
  request.set_user_id(userId);
  *request.mutable_frame() = toFrame(frame);

  argus::sync::v1::ControlAck ack;
  return answer(stub_->EmitToUser(&context, request, &ack), ack);
}
