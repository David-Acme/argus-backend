#include "catalog-replica.hxx"

#include <drogon/drogon.h>
#include <sync/module-audit-event.hxx>
#include <sync/sync-operation.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <text/json-util.hxx>
#include <nats/nats-subject.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <trantor/utils/Logger.h>

#include <ctime>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

constexpr int kMaxDeliver = 50;

constexpr const char* UPSERT_PERSON =
    "INSERT INTO catalog_person (id, user_id, name, alias, deleted_at) "
    "VALUES (?, ?, ?, ?, NULL) "
    "ON CONFLICT(id) DO UPDATE SET user_id = excluded.user_id, "
    "name = excluded.name, alias = excluded.alias, deleted_at = NULL";

constexpr const char* TOMBSTONE_PERSON =
    "UPDATE catalog_person SET deleted_at = ? WHERE id = ?";

constexpr const char* UPSERT_CAMERA =
    "INSERT INTO catalog_camera (id, name, deleted_at) VALUES (?, ?, NULL) "
    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, deleted_at = NULL";

constexpr const char* TOMBSTONE_CAMERA =
    "UPDATE catalog_camera SET deleted_at = ? WHERE id = ?";

constexpr const char* UPSERT_ZONE =
    "INSERT INTO catalog_zone (id, name) VALUES (?, ?) "
    "ON CONFLICT(id) DO UPDATE SET name = excluded.name";

constexpr const char* DELETE_ZONE = "DELETE FROM catalog_zone WHERE id = ?";

constexpr const char* UPSERT_STREAM =
    "INSERT INTO catalog_stream (id, label) VALUES (?, ?) "
    "ON CONFLICT(id) DO UPDATE SET label = excluded.label";

constexpr const char* DELETE_STREAM =
    "DELETE FROM catalog_stream WHERE id = ?";

struct StmtExecInput
{
  const char* sql;
  std::function<void(SqliteStmt&)> bind;
};

bool execStmt(sqlite3* db, const StmtExecInput& input)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, input.sql))
    return false;
  if (input.bind)
    input.bind(stmt);
  return stmt.step() == SQLITE_DONE;
}

void applyStmt(sqlite3* db, const StmtExecInput& input)
{
  if (!execStmt(db, input))
    throw std::runtime_error(std::string("catalog replica write failed: ") +
                             sqlite3_errmsg(db));
}

bool replicaPopulated(sqlite3* db, const char* table)
{
  SqliteStmt probe;
  const std::string count = std::string("SELECT COUNT(*) FROM ") + table;
  if (!probe.prepare(db, count.c_str()))
    return false;
  return probe.step() == SQLITE_ROW && probe.columnInt64(0) > 0;
}

}

namespace catalog_feed
{
const std::vector<Feed>& defaults()
{
  static const std::vector<Feed> feeds{
      {.stream = nats_subject::kCameraStream,
       .subject = nats_subject::kCameraChange,
       .durable = "argus-llm-catalog-camera"},
      {.stream = nats_subject::kIdentityChangeStream,
       .subject = nats_subject::kIdentityChange,
       .durable = "argus-llm-catalog-identity"},
  };
  return feeds;
}
}

CatalogReplica::CatalogReplica(const Deps& deps)
    : bus_(deps.bus), graph_(deps.graph), resolver_(deps.resolver)
{
  attachments_.reserve(catalog_feed::defaults().size());
  for (const auto& feed : catalog_feed::defaults())
    attachments_.push_back(Attachment{.feed = feed, .subscription = {}});
}

CatalogReplica::~CatalogReplica()
{
  stop();
}

void CatalogReplica::subscribe()
{
  if (subscribePending())
    return;
  LOG_WARN << "Catalog replica: change streams not ready; retrying";
  scheduleSubscribeRetry();
}

void CatalogReplica::stop()
{
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  for (auto& attachment : attachments_) {
    if (attachment.subscription.has_value())
      bus_.unsubscribe(*attachment.subscription);
    attachment.subscription.reset();
  }
}

void CatalogReplica::applyPayload(const ApplyInput& input)
{
  try {
    const Json::Value json = json_util::fromString(input.payload);
    if (input.subject == nats_subject::kIdentityChange)
      applyIdentity(json);
    else
      applyCamera(json);
    if (input.settlement.ack)
      input.settlement.ack();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Catalog replica: redelivering (" << error.what() << ")";
    if (input.settlement.nak)
      input.settlement.nak();
  }
}

bool CatalogReplica::trySubscribe(Attachment& attachment)
{
  const auto subscription = bus_.subscribeDurable(
      {.stream = attachment.feed.stream,
       .durable = attachment.feed.durable,
       .subject = attachment.feed.subject,
       .deliverAll = true,
       .maxDeliver = kMaxDeliver,
       .handler = [this](const NatsBus::DurableMessage& message,
                         NatsBus::DurableSettlement settlement) {
         ApplyInput input{.subject = std::string(message.subject),
                          .payload = std::string(message.payload),
                          .settlement = std::move(settlement)};
         drogon::app().getIOLoop(0)->runInLoop(
             [this, input = std::move(input)]() mutable {
               applyPayload(input);
             });
       }});
  if (!subscription)
    return false;
  attachment.subscription = subscription;
  return true;
}

bool CatalogReplica::subscribePending()
{
  bool allAttached = true;
  for (auto& attachment : attachments_) {
    if (attachment.subscription.has_value())
      continue;
    if (trySubscribe(attachment))
      LOG_INFO << "Catalog replica: durable " << attachment.feed.durable
               << " connected on " << attachment.feed.subject;
    else
      allAttached = false;
  }
  return allAttached;
}

void CatalogReplica::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(5.0, [this]() {
    if (!subscribePending() || !retryTimer_.has_value())
      return;
    drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  });
}

void CatalogReplica::applyIdentity(const Json::Value& event)
{
  if (!event.isObject() || event.get("kind", "").asString() != "identity")
    return;
  const std::string table = event.get("table", "").asString();
  if (table != "person")
    return;

  const int64_t id = event.get("id", 0).asInt64();
  if (id <= 0)
    return;
  const bool deleted = event.get("deleted", false).asBool();
  const Json::Value row = event.get("row", Json::Value(Json::objectValue));

  {
    std::scoped_lock lock(graph_.mutex());
    if (deleted) {
      applyStmt(graph_.handle(), {.sql = TOMBSTONE_PERSON, .bind = [&](SqliteStmt& stmt) {
                 stmt.bindInt64(1, std::time(nullptr));
                 stmt.bindInt64(2, id);
               }});
    }
    else {
      applyStmt(graph_.handle(), {.sql = UPSERT_PERSON, .bind = [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindInt64(2, row.get("user_id", 0).asInt64());
        stmt.bindText(3, row.get("name", "").asString());
        stmt.bindText(4, row.get("alias", "").asString());
      }});
    }
  }
  resolver_.build();
  LOG_DEBUG << "CatalogReplica: identity " << table << " " << id
            << (deleted ? " tombstoned" : " upserted");
}

void CatalogReplica::applyCamera(const Json::Value& event)
{
  if (!event.isObject())
    return;

  if (event.get("kind", "").asString() == "audit") {
    const auto audit = ModuleAuditEvent::fromJson(event);
    if (!audit)
      return;
    const std::string table = tableNameToString(audit->tableName);
    const auto nameIt = audit->changes.find("name");
    const auto labelIt = audit->changes.find("label");
    const auto deletedIt = audit->changes.find("deleted_at");
    {
      std::scoped_lock lock(graph_.mutex());
      if (deletedIt != audit->changes.end()) {
        if (table == "camera")
          applyStmt(graph_.handle(), {.sql = TOMBSTONE_CAMERA, .bind = [&](SqliteStmt& stmt) {
            stmt.bindInt64(1, std::time(nullptr));
            stmt.bindInt64(2, audit->recordId);
          }});
        else if (table == "zone")
          applyStmt(graph_.handle(), {.sql = DELETE_ZONE, .bind = [&](SqliteStmt& stmt) { stmt.bindInt64(1, audit->recordId); }});
        else if (table == "camera_stream")
          applyStmt(graph_.handle(), {.sql = DELETE_STREAM, .bind = [&](SqliteStmt& stmt) { stmt.bindInt64(1, audit->recordId); }});
      }
      else if (table == "camera" && nameIt != audit->changes.end()) {
        applyStmt(graph_.handle(), {.sql = UPSERT_CAMERA, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, nameIt->second.current.asString());
        }});
      }
      else if (table == "zone" && nameIt != audit->changes.end()) {
        applyStmt(graph_.handle(), {.sql = UPSERT_ZONE, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, nameIt->second.current.asString());
        }});
      }
      else if (table == "camera_stream" && labelIt != audit->changes.end()) {
        applyStmt(graph_.handle(), {.sql = UPSERT_STREAM, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, labelIt->second.current.asString());
        }});
      }
      else {
        return;
      }
    }
    resolver_.build();
    return;
  }

  const auto operation = static_cast<SyncOperation>(
      event.get("operation", 0).asInt());
  const std::string option = event.get("option", "").asString();
  if (operation != SyncOperation::Add && operation != SyncOperation::Delete)
    return;
  const Json::Value row = event.get("info", Json::Value(Json::objectValue));
  const int64_t id = row.get("id", 0).asInt64();
  if (id <= 0)
    return;

  {
    std::scoped_lock lock(graph_.mutex());
    if (operation == SyncOperation::Delete) {
      if (option == "camera")
        applyStmt(graph_.handle(), {.sql = TOMBSTONE_CAMERA, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1,
                         row.get("deletedAt", std::time(nullptr)).asInt64());
          stmt.bindInt64(2, id);
        }});
      else if (option == "zone")
        applyStmt(graph_.handle(), {.sql = DELETE_ZONE, .bind = [&](SqliteStmt& stmt) { stmt.bindInt64(1, id); }});
      else if (option == "camera_stream")
        applyStmt(graph_.handle(), {.sql = DELETE_STREAM, .bind = [&](SqliteStmt& stmt) { stmt.bindInt64(1, id); }});
      else
        return;
    }
    else if (option == "camera") {
      applyStmt(graph_.handle(), {.sql = UPSERT_CAMERA, .bind = [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("name", "").asString());
      }});
    }
    else if (option == "zone") {
      applyStmt(graph_.handle(), {.sql = UPSERT_ZONE, .bind = [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("name", "").asString());
      }});
    }
    else if (option == "camera_stream") {
      applyStmt(graph_.handle(), {.sql = UPSERT_STREAM, .bind = [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("label", "").asString());
      }});
    }
    else {
      return;
    }
  }
  resolver_.build();
}

void CatalogReplica::seedFromSnapshot(const Snapshot& snapshot)
{
  seedSnapshot({graph_, resolver_, snapshot});
}

void CatalogReplica::seedSnapshot(const SnapshotSources& sources)
{
  int64_t persons = 0;
  int64_t cameras = 0;
  int64_t zones = 0;
  int64_t streams = 0;
  {
    std::scoped_lock lock(sources.graph.mutex());
    sqlite3* db = sources.graph.handle();
    if (!db)
      return;
    const bool personsEmpty = !replicaPopulated(db, "catalog_person");
    const bool camerasEmpty = !replicaPopulated(db, "catalog_camera");
    const bool zonesEmpty = !replicaPopulated(db, "catalog_zone");
    const bool streamsEmpty = !replicaPopulated(db, "catalog_stream");
    if (!personsEmpty && !camerasEmpty && !zonesEmpty && !streamsEmpty) {
      LOG_INFO << "CatalogReplica: replicas already populated; snapshot "
                  "fill skipped";
      return;
    }
    if (personsEmpty) {
      for (const auto& row : sources.snapshot.persons) {
        execStmt(db, {.sql = UPSERT_PERSON, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row.id);
          stmt.bindInt64(2, row.userId);
          stmt.bindText(3, row.name);
          stmt.bindText(4, row.alias);
        }});
        ++persons;
      }
    }
    if (camerasEmpty) {
      for (const auto& row : sources.snapshot.cameras) {
        execStmt(db, {.sql = UPSERT_CAMERA, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row.id);
          stmt.bindText(2, row.name);
        }});
        ++cameras;
      }
    }
    if (zonesEmpty) {
      for (const auto& row : sources.snapshot.zones) {
        execStmt(db, {.sql = UPSERT_ZONE, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row.id);
          stmt.bindText(2, row.name);
        }});
        ++zones;
      }
    }
    if (streamsEmpty) {
      for (const auto& row : sources.snapshot.streams) {
        execStmt(db, {.sql = UPSERT_STREAM, .bind = [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row.id);
          stmt.bindText(2, row.label);
        }});
        ++streams;
      }
    }
  }
  sources.resolver.build();
  LOG_INFO << "CatalogReplica: snapshot filled persons=" << persons
           << " cameras=" << cameras << " zones=" << zones
           << " streams=" << streams;
}
