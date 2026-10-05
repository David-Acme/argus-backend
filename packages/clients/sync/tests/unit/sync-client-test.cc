#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/sync/v1/sync.grpc.pb.h>
#include <array>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-client.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <utility>

namespace
{

constexpr const char* kFleetSecret = "fleet-secret";

namespace v1 = argus::sync::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

struct ScriptedCalls
{
  int replaceRoleRooms = 0;
  int disconnectUser = 0;
  int emitToUser = 0;
};

class ScriptedSyncService final
    : public v1::SyncControlService::CallbackService
{
public:
  void setAck(bool ok, const std::string& reason = {})
  {
    const std::scoped_lock lock(mutex_);
    ack_.set_ok(ok);
    ack_.set_reason(reason);
  }

  ScriptedCalls calls() const
  {
    const std::scoped_lock lock(mutex_);
    return calls_;
  }

  std::map<std::string, std::string> seen() const
  {
    const std::scoped_lock lock(mutex_);
    return seen_;
  }

  v1::ReplaceRoleRoomsRequest roles() const
  {
    const std::scoped_lock lock(mutex_);
    return roles_;
  }

  v1::SyncFrame disconnectFrame() const
  {
    const std::scoped_lock lock(mutex_);
    return disconnectFrame_;
  }

  v1::SyncFrame emitFrame() const
  {
    const std::scoped_lock lock(mutex_);
    return emitFrame_;
  }

  Reactor* ReplaceRoleRooms(Ctx* context, const v1::ReplaceRoleRoomsRequest* in,
                            v1::ControlAck* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      roles_ = *in;
      ++calls_.replaceRoleRooms;
    }
    return answer(context, out);
  }

  Reactor* DisconnectUser(Ctx* context, const v1::DisconnectUserRequest* in,
                          v1::ControlAck* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      disconnectFrame_ = in->frame();
      ++calls_.disconnectUser;
    }
    return answer(context, out);
  }

  Reactor* EmitToUser(Ctx* context, const v1::EmitToUserRequest* in,
                      v1::ControlAck* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      emitFrame_ = in->frame();
      ++calls_.emitToUser;
    }
    return answer(context, out);
  }

private:
  Reactor* answer(Ctx* context, v1::ControlAck* out)
  {
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : context->client_metadata())
      headers.emplace(std::string(key.begin(), key.end()),
                      std::string(value.begin(), value.end()));
    {
      const std::scoped_lock lock(mutex_);
      seen_ = std::move(headers);
      *out = ack_;
    }
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  mutable std::mutex mutex_;
  v1::ControlAck ack_;
  ScriptedCalls calls_;
  std::map<std::string, std::string> seen_;
  v1::ReplaceRoleRoomsRequest roles_;
  v1::SyncFrame disconnectFrame_;
  v1::SyncFrame emitFrame_;
};

std::unique_ptr<grpc::Server> startServer(ScriptedSyncService& service,
                                          std::string& target)
{
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = std::unique_ptr<grpc::Server>(builder.BuildAndStart());
  target = "127.0.0.1:" + std::to_string(port);
  return server;
}

SocketEmitDto authContextRow(int64_t userId, bool resync)
{
  SocketEmitDto row;
  row.operation = SyncOperation::AuthContextChanged;
  row.option = TableName::User;
  row.obj["id"] = static_cast<Json::Int64>(userId);
  row.obj["resync"] = resync;
  return row;
}

SocketEmitDto auditLogRow(int64_t userId)
{
  SocketEmitDto row;
  row.operation = SyncOperation::Log;
  row.option = TableName::UserAuditLog;
  row.obj["id"] = 7;
  row.obj["userId"] = static_cast<Json::Int64>(userId);
  return row;
}

}

TEST_CASE("an id the client cannot resolve never reaches sync")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const SyncClient client({.target = target, .fleetSecret = kFleetSecret});
  service.setAck(true);

  CHECK_FALSE(client.replaceRoleRooms(
      {.userId = 0, .oldRole = "resident", .newRole = "guard"}));
  CHECK_FALSE(client.disconnectUser(0, authContextRow(0, false)));
  CHECK_FALSE(client.emitToUser(-1, authContextRow(-1, true)));
  const auto refused = service.calls();
  CHECK(refused.replaceRoleRooms == 0);
  CHECK(refused.disconnectUser == 0);
  CHECK(refused.emitToUser == 0);

  CHECK(client.replaceRoleRooms(
      {.userId = 12, .oldRole = "resident", .newRole = "guard"}));
  CHECK(client.disconnectUser(12, authContextRow(12, false)));
  CHECK(client.emitToUser(12, auditLogRow(12)));
  const auto landed = service.calls();
  CHECK(landed.replaceRoleRooms == 1);
  CHECK(landed.disconnectUser == 1);
  CHECK(landed.emitToUser == 1);

  server->Shutdown();
}

TEST_CASE("the fleet secret and the frozen frame are what this edge sends")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const SyncClient client({.target = target, .fleetSecret = kFleetSecret});
  service.setAck(true);

  CHECK(client.replaceRoleRooms(
      {.userId = 12, .oldRole = "resident", .newRole = "guard"}));
  const auto roles = service.roles();
  CHECK(roles.user_id() == 12);
  CHECK(roles.old_role() == "resident");
  CHECK(roles.new_role() == "guard");

  const auto context = authContextRow(12, false);
  CHECK(client.disconnectUser(12, context));
  const auto dropped = service.disconnectFrame();
  CHECK(dropped.operation() == v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED);
  CHECK(dropped.table() == v1::TABLE_NAME_USER);
  CHECK(dropped.info() == json_util::toString(context.obj));
  CHECK_FALSE(json_util::fromString(dropped.info())["resync"].asBool());

  const auto log = auditLogRow(12);
  CHECK(client.emitToUser(12, log));
  const auto emitted = service.emitFrame();
  CHECK(emitted.operation() == v1::SYNC_OPERATION_LOG);
  CHECK(emitted.table() == v1::TABLE_NAME_USER_AUDIT_LOG);
  CHECK(emitted.info() == json_util::toString(log.obj));
  CHECK(json_util::fromString(emitted.info())["userId"].asInt64() == 12);

  const auto presented = service.seen();
  CHECK(presented.at("x-argus-fleet") == kFleetSecret);

  server->Shutdown();
}

TEST_CASE("a paired client presents its own credential and never the fleet "
          "secret")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const SyncClient client({.target = target,
                           .credential = "identity-credential",
                           .fleetSecret = kFleetSecret});
  service.setAck(true);

  CHECK(client.replaceRoleRooms(
      {.userId = 12, .oldRole = "resident", .newRole = "guard"}));
  const auto presented = service.seen();
  CHECK(presented.at("x-argus-credential") == "identity-credential");
  CHECK(presented.count("x-argus-fleet") == 0);

  server->Shutdown();
}

TEST_CASE("the server's refusal is the answer, not the transport")
{
  ScriptedSyncService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const SyncClient client({.target = target, .fleetSecret = kFleetSecret});
  service.setAck(false, "no such room");

  CHECK_FALSE(client.replaceRoleRooms(
      {.userId = 12, .oldRole = "resident", .newRole = "guard"}));
  CHECK_FALSE(client.disconnectUser(12, authContextRow(12, false)));
  CHECK_FALSE(client.emitToUser(12, auditLogRow(12)));
  const auto landed = service.calls();
  CHECK(landed.replaceRoleRooms == 1);
  CHECK(landed.disconnectUser == 1);
  CHECK(landed.emitToUser == 1);
  CHECK(service.seen().at("x-argus-fleet") == kFleetSecret);

  server->Shutdown();
}

TEST_CASE("an unreachable sync is a refusal, not a crash")
{
  const SyncClient client(
      {.target = "127.0.0.1:1", .fleetSecret = kFleetSecret});

  CHECK_FALSE(client.disconnectUser(12, authContextRow(12, false)));
}

TEST_CASE("both frozen enums travel value for value")
{
  const std::array<std::pair<TableName, v1::TableName>, 24> tables = {{
      {TableName::User, v1::TABLE_NAME_USER},
      {TableName::UserInvitation, v1::TABLE_NAME_USER_INVITATION},
      {TableName::Person, v1::TABLE_NAME_PERSON},
      {TableName::PersonEvent, v1::TABLE_NAME_PERSON_EVENT},
      {TableName::Event, v1::TABLE_NAME_EVENT},
      {TableName::Reminder, v1::TABLE_NAME_REMINDER},
      {TableName::ReminderDetail, v1::TABLE_NAME_REMINDER_DETAIL},
      {TableName::CalendarEvent, v1::TABLE_NAME_CALENDAR_EVENT},
      {TableName::CalendarEventShare, v1::TABLE_NAME_CALENDAR_EVENT_SHARE},
      {TableName::Project, v1::TABLE_NAME_PROJECT},
      {TableName::ProjectMember, v1::TABLE_NAME_PROJECT_MEMBER},
      {TableName::ProjectTask, v1::TABLE_NAME_PROJECT_TASK},
      {TableName::ContextNote, v1::TABLE_NAME_CONTEXT_NOTE},
      {TableName::Camera, v1::TABLE_NAME_CAMERA},
      {TableName::CameraStream, v1::TABLE_NAME_CAMERA_STREAM},
      {TableName::Zone, v1::TABLE_NAME_ZONE},
      {TableName::AuditLog, v1::TABLE_NAME_AUDIT_LOG},
      {TableName::UserAuditLog, v1::TABLE_NAME_USER_AUDIT_LOG},
      {TableName::Notification, v1::TABLE_NAME_NOTIFICATION},
      {TableName::NotificationToken, v1::TABLE_NAME_NOTIFICATION_TOKEN},
      {TableName::UserActionLog, v1::TABLE_NAME_USER_ACTION_LOG},
      {TableName::RefreshToken, v1::TABLE_NAME_REFRESH_TOKEN},
      {TableName::FaceEmbedding, v1::TABLE_NAME_FACE_EMBEDDING},
      {TableName::Memory, v1::TABLE_NAME_MEMORY},
  }};
  for (const auto& [cxx, wire] : tables)
    CHECK_MESSAGE(static_cast<int>(cxx) == static_cast<int>(wire),
                  "table ", static_cast<int>(cxx));
  CHECK(static_cast<int>(kLastTableName) == 23);

  const std::array<std::pair<SyncOperation, v1::SyncOperation>, 8> operations =
      {{
          {SyncOperation::InitialInfo, v1::SYNC_OPERATION_INITIAL_INFO},
          {SyncOperation::Synchronize, v1::SYNC_OPERATION_SYNCHRONIZE},
          {SyncOperation::SynchronizeAuditLog,
           v1::SYNC_OPERATION_SYNCHRONIZE_AUDIT_LOG},
          {SyncOperation::SynchronizeUserAuditLog,
           v1::SYNC_OPERATION_SYNCHRONIZE_USER_AUDIT_LOG},
          {SyncOperation::Add, v1::SYNC_OPERATION_ADD},
          {SyncOperation::Delete, v1::SYNC_OPERATION_DELETE},
          {SyncOperation::Log, v1::SYNC_OPERATION_LOG},
          {SyncOperation::AuthContextChanged,
           v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED},
      }};
  for (const auto& [cxx, wire] : operations)
    CHECK_MESSAGE(static_cast<int>(cxx) == static_cast<int>(wire),
                  "operation ", static_cast<int>(cxx));
}
