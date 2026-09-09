#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>

#include <string>

namespace
{
SocketEmitDto emitDto(SyncOperation operation, TableName table)
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = table;
  body.obj["id"] = 7;
  return body;
}
} // namespace

TEST_CASE("module emit payload carries the bare SocketEmitDto triple")
{
  const Json::Value payload =
      sync_change::emitPayload(emitDto(SyncOperation::Add, TableName::Camera));

  CHECK(payload["operation"] == 4);
  CHECK(payload["option"] == "camera");
  CHECK(payload["info"]["id"] == 7);
  CHECK_FALSE(payload.isMember("users"));
  CHECK_FALSE(payload.isMember("action"));
}

TEST_CASE("user emit payload always carries the users routing field")
{
  const Json::Value scoped =
      sync_change::userEmitPayload(emitDto(SyncOperation::Add, TableName::User),
                                   {42, 43});
  CHECK(scoped["users"].size() == 2);
  CHECK(scoped["users"][0] == 42);

  const Json::Value empty =
      sync_change::userEmitPayload(emitDto(SyncOperation::Add, TableName::User),
                                   {});
  CHECK(empty["users"].isArray());
  CHECK(empty["users"].empty());
}

TEST_CASE("disconnect and role-room payloads carry the room-control action")
{
  const Json::Value disconnect = sync_change::disconnectPayload(
      emitDto(SyncOperation::AuthContextChanged, TableName::User), 42);
  CHECK(disconnect["action"] == "disconnect");
  CHECK(disconnect["users"][0] == 42);
  CHECK(disconnect["operation"] == 7);
  CHECK(disconnect["user"] == 42);

  RoleRoomReplaceInput input;
  input.userId = 42;
  input.oldRole = UserRole::Resident;
  input.newRole = UserRole::Guest;
  const Json::Value roleRooms = sync_change::roleRoomsPayload(input);
  CHECK(roleRooms["action"] == "replace_role_rooms");
  CHECK(roleRooms["operation"] == 7);
  CHECK(roleRooms["option"] == "user");
  CHECK(roleRooms["info"]["id"] == 42);
  CHECK(roleRooms["user"] == 42);
  CHECK(roleRooms["old_role"] == "resident");
  CHECK(roleRooms["new_role"] == "guest");
}

TEST_CASE("payloads re-serialize to the client wire triple")
{
  const SocketEmitDto body =
      emitDto(SyncOperation::Delete, TableName::Reminder);
  const Json::Value payload = sync_change::emitPayload(body, {9});

  SocketEmitDto rebuilt;
  rebuilt.operation = static_cast<SyncOperation>(payload["operation"].asInt());
  rebuilt.option = tableNameFromString(payload["option"].asString());
  rebuilt.obj = payload["info"];
  CHECK(json_util::toString(rebuilt.toJson()) ==
        json_util::toString(body.toJson()));
}

TEST_CASE("sync-change subject follows the argus.<domain>.v1.change contract")
{
  const std::string subject = nats_subject::kSyncChange;
  CHECK(subject == "argus.sync.v1.change");

  const std::string wildcard = nats_subject::kSyncChangeWildcard;
  CHECK(wildcard == "argus.*.v1.change");
  CHECK(subject.find("argus.") == 0);
  CHECK(subject.find(".v1.change") == subject.size() - 10);
}
