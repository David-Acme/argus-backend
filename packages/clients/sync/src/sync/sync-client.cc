#include "sync-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <text/json-util.hxx>

namespace
{
constexpr int kCallTimeoutMs = 5000;

// The caller holds a SocketEmitDto -- operation, table, row -- and the wire
// carries it typed: the two enums as the numbers they carry, the row as JSON
// text. The numbers agree because contracts.proto mirrors sync-operation.hxx
// and table-name.hxx value for value, and the suite pins that equality. The
// envelope's own spelling (`option` as the table's name, `info` as the object)
// is rebuilt at the far end, where the frame is rendered.
argus::sync::v1::SyncFrame toFrame(const SocketEmitDto& dto)
{
  argus::sync::v1::SyncFrame frame;
  frame.set_operation(
      static_cast<argus::sync::v1::SyncOperation>(dto.operation));
  frame.set_table(static_cast<argus::sync::v1::TableName>(dto.option));
  frame.set_info(json_util::toString(dto.obj));
  return frame;
}

// The ack leg every method shares: false when the call never landed, and the
// server's own refusal when it did.
bool answer(const grpc::Status& status, const argus::sync::v1::ControlAck& ack)
{
  return status.ok() && ack.ok();
}
} // namespace

SyncClient::SyncClient(SyncClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::sync::v1::SyncControlService::NewStub(channel_)),
      fleetSecret_(std::move(config.fleetSecret))
{
}

bool SyncClient::replaceRoleRooms(
    const sync_change::RoleRoomChange& change) const
{
  if (change.userId <= 0)
    return false;

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addFleetSecret(context, fleetSecret_);

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
  argus::client::addFleetSecret(context, fleetSecret_);

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
  argus::client::addFleetSecret(context, fleetSecret_);

  argus::sync::v1::EmitToUserRequest request;
  request.set_user_id(userId);
  *request.mutable_frame() = toFrame(frame);

  argus::sync::v1::ControlAck ack;
  return answer(stub_->EmitToUser(&context, request, &ack), ack);
}
