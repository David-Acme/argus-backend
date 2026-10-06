#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/orm/DbClient.h>
#include <feature/modules/services/module-journal.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

class RecordingSink final : public ModuleActionSink
{
public:
  std::mutex mutex;
  std::vector<ModuleActionRecord> records;
  bool online{true};
  int refusals{0};

  bool publish(const ModuleActionRecord& record) override
  {
    const std::scoped_lock lock(mutex);
    if (!online) {
      ++refusals;
      return false;
    }
    records.push_back(record);
    return true;
  }
};

class JournalDb
{
public:
  JournalDb()
      : path_(fs::temp_directory_path() /
              ("argus-module-journal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db"))
  {
    db = open();
    REQUIRE(DbService::runScriptFile(ARGUS_SETTINGS_SCHEMA_FILE, db));
  }

  ~JournalDb()
  {
    db.reset();
    std::error_code error;
    fs::remove(path_, error);
    fs::remove(path_.string() + "-wal", error);
    fs::remove(path_.string() + "-shm", error);
  }

  JournalDb(const JournalDb&) = delete;
  JournalDb& operator=(const JournalDb&) = delete;

  [[nodiscard]] drogon::orm::DbClientPtr open() const
  {
    return drogon::orm::DbClient::newSqlite3Client("filename=" + path_.string(), 1);
  }

  std::int64_t audit(const std::string& module, ModuleAuditAction action, std::int64_t user = 1,
                                   const std::string& detail = {}) const
  {
    return ModuleAuditRepository(db)
        .create({.moduleId = module, .action = action, .userId = user, .detail = detail, .at = 1700000000})
        .id;
  }

  drogon::orm::DbClientPtr db;

private:
  fs::path path_;
};
}

TEST_CASE("a journal starting on an old history skips it and publishes only what follows, in order")
{
  JournalDb fixture;
  fixture.audit("surveillance", ModuleAuditAction::Enabled);
  fixture.audit("productivity", ModuleAuditAction::Disabled);

  RecordingSink sink;
  ModuleJournal journal({.db = fixture.db, .sink = &sink, .pollInterval = std::chrono::milliseconds(50)});
  CHECK(journal.relay() == 0);

  const auto first = fixture.audit("surveillance", ModuleAuditAction::Disabled, 7);
  const auto second = fixture.audit("surveillance", ModuleAuditAction::UninstallRequested, 7, "keep_data");
  CHECK(journal.relay() == 2);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].auditId == first);
  CHECK(sink.records[1].auditId == second);
  CHECK(sink.records[0].event.userId == 7);
  CHECK(sink.records[0].event.module == "surveillance");
  CHECK(sink.records[0].event.subject == "module");
  CHECK(sink.records[0].event.newData["event"].asString() == "disabled");
  CHECK(sink.records[0].event.newData["lifecycle"].asString() == "disabled");
  CHECK(sink.records[1].event.newData["detail"].asString() == "keep_data");
  CHECK(module_action::msgId(first) == "settings-action:" + std::to_string(first));

  CHECK(journal.relay() == 0);
  CHECK(sink.records.size() == 2);
}

TEST_CASE("a sink that refuses stops the pass without skipping and the next pass resumes at the same entry")
{
  JournalDb fixture;
  RecordingSink sink;
  ModuleJournal journal({.db = fixture.db, .sink = &sink, .pollInterval = std::chrono::milliseconds(50)});

  const auto one = fixture.audit("productivity", ModuleAuditAction::Enabled);
  const auto two = fixture.audit("productivity", ModuleAuditAction::Disabled);
  const auto three = fixture.audit("productivity", ModuleAuditAction::Enabled);

  sink.online = false;
  CHECK(journal.relay() == 0);
  CHECK(sink.refusals == 1);
  CHECK(sink.records.empty());

  sink.online = true;
  CHECK(journal.relay() == 3);
  REQUIRE(sink.records.size() == 3);
  CHECK(sink.records[0].auditId == one);
  CHECK(sink.records[1].auditId == two);
  CHECK(sink.records[2].auditId == three);
}

TEST_CASE("the cursor is persisted: a restarted journal neither repeats nor misses an entry")
{
  JournalDb fixture;
  RecordingSink sink;
  {
    ModuleJournal journal({.db = fixture.db, .sink = &sink, .pollInterval = std::chrono::milliseconds(50)});
    fixture.audit("surveillance", ModuleAuditAction::Enabled);
    CHECK(journal.relay() == 1);
  }

  const auto missed = fixture.audit("surveillance", ModuleAuditAction::Disabled);
  ModuleJournal restarted({.db = fixture.open(), .sink = &sink, .pollInterval = std::chrono::milliseconds(50)});
  CHECK(restarted.relay() == 1);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[1].auditId == missed);
  CHECK(restarted.relay() == 0);
}

TEST_CASE("a batch smaller than the backlog is drained over several passes")
{
  JournalDb fixture;
  RecordingSink sink;
  ModuleJournal journal({.db = fixture.db, .sink = &sink, .pollInterval = std::chrono::milliseconds(50), .batch = 2});
  for (int index = 0; index < 5; ++index)
    fixture.audit("productivity", ModuleAuditAction::Paused);
  CHECK(journal.relay() == 2);
  CHECK(journal.relay() == 2);
  CHECK(journal.relay() == 1);
  CHECK(journal.relay() == 0);
  CHECK(sink.records.size() == 5);
}

TEST_CASE("every audit action becomes a module action the activity history can read")
{
  struct Row
  {
    ModuleAuditAction audit;
    UserAction action;
    const char* event;
    const char* lifecycle;
  };
  const std::vector<Row> rows{
      {.audit = ModuleAuditAction::Adopted, .action = UserAction::Create, .event = "adopted", .lifecycle = ""},
      {.audit = ModuleAuditAction::InstallRequested, .action = UserAction::Create, .event = "install_requested", .lifecycle = ""},
      {.audit = ModuleAuditAction::Enabled, .action = UserAction::Update, .event = "enabled", .lifecycle = "active"},
      {.audit = ModuleAuditAction::Disabled, .action = UserAction::Update, .event = "disabled", .lifecycle = "disabled"},
      {.audit = ModuleAuditAction::RolledBack, .action = UserAction::Update, .event = "rolled_back", .lifecycle = "disabled"},
      {.audit = ModuleAuditAction::Paused, .action = UserAction::Update, .event = "paused", .lifecycle = ""},
      {.audit = ModuleAuditAction::Resumed, .action = UserAction::Update, .event = "resumed", .lifecycle = ""},
      {.audit = ModuleAuditAction::Cancelled, .action = UserAction::Update, .event = "cancelled", .lifecycle = ""},
      {.audit = ModuleAuditAction::Failed, .action = UserAction::Update, .event = "failed", .lifecycle = ""},
      {.audit = ModuleAuditAction::UninstallRequested, .action = UserAction::Delete, .event = "uninstall_requested", .lifecycle = ""},
      {.audit = ModuleAuditAction::Removed, .action = UserAction::Delete, .event = "removed", .lifecycle = ""},
      {.audit = ModuleAuditAction::Purged, .action = UserAction::Delete, .event = "purged", .lifecycle = "not_installed"},
  };
  for (const auto& row : rows) {
    CAPTURE(row.event);
    const auto event = module_journal::eventOf({.id = 1,
                                                .moduleId = "surveillance",
                                                .action = row.audit,
                                                .userId = 4,
                                                .detail = {},
                                                .createdAt = 1700000123});
    CHECK(event.action == row.action);
    CHECK(event.newData["event"].asString() == row.event);
    CHECK(event.newData.isMember("lifecycle") == (std::string(row.lifecycle) != ""));
    CHECK(event.newData["lifecycle"].asString() == row.lifecycle);
    CHECK(event.newData["at"].asInt64() == 1700000123);
    CHECK_FALSE(event.newData.isMember("detail"));
    CHECK(event.recordId == 0);

    const auto wire = UserActionEvent::fromJson(event.toJson());
    CHECK(wire.has_value());
    CHECK(wire.value_or(UserActionEvent{}).subject == "module");
    CHECK(wire.value_or(UserActionEvent{}).module == "surveillance");
  }
}

TEST_CASE("an uninstall request that moved people to new roles journals who moved from which role to which")
{
  const auto event = module_journal::eventOf({.id = 9,
                                              .moduleId = "surveillance",
                                              .action = ModuleAuditAction::UninstallRequested,
                                              .userId = 1,
                                              .detail = "keep_data;moved=7:guard>resident,8:guard>guest",
                                              .createdAt = 1700000200});
  CHECK(event.newData["event"].asString() == "uninstall_requested");
  CHECK(event.newData["detail"].asString() == "keep_data");
  REQUIRE(event.newData["roleMoves"].size() == 2);
  CHECK(event.newData["roleMoves"][0]["userId"].asInt64() == 7);
  CHECK(event.newData["roleMoves"][0]["from"].asString() == "guard");
  CHECK(event.newData["roleMoves"][0]["to"].asString() == "resident");
  CHECK(event.newData["roleMoves"][1]["to"].asString() == "guest");
  CHECK(event.userId == 1);

  const auto plain = module_journal::eventOf({.id = 10,
                                              .moduleId = "surveillance",
                                              .action = ModuleAuditAction::UninstallRequested,
                                              .userId = 1,
                                              .detail = "purge",
                                              .createdAt = 1700000201});
  CHECK(plain.newData["detail"].asString() == "purge");
  CHECK_FALSE(plain.newData.isMember("roleMoves"));

  const auto torn = module_journal::eventOf({.id = 11,
                                             .moduleId = "surveillance",
                                             .action = ModuleAuditAction::UninstallRequested,
                                             .userId = 1,
                                             .detail = "keep_data;moved=x:guard>resident,9guard>guest,3:>",
                                             .createdAt = 1700000202});
  CHECK(torn.newData["detail"].asString() == "keep_data");
  CHECK(torn.newData["roleMoves"].size() == 1);
}
