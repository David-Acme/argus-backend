#include <doctest/doctest.h>

#include "support/fake-messenger-ports.hxx"

#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <feature/messenger/infra/graph-messenger-store.hxx>
#include <feature/messenger/services/relay-service.hxx>
#include <sqlite/schema-runner.hxx>

#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

const std::vector<relay_recipient::Member> kRoster{{.userId = 1, .name = "David Acme"}, {.userId = 2, .name = "Ana Lopez"}};

int64_t scalar(sqlite3* db, const char* sql)
{
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
    return -1;
  int64_t value = -1;
  if (sqlite3_step(statement) == SQLITE_ROW)
    value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

std::string text(sqlite3* db, const char* sql)
{
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
    return "prepare failed";
  std::string value;
  if (sqlite3_step(statement) == SQLITE_ROW)
    value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  return value;
}

}

TEST_CASE("relays and memory writes share the graph's connection and lock: no busy error and an intact database")
{
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "argus-llm-messenger-concurrency.db";
  std::filesystem::remove(path);
  SqliteGraph graph;
  REQUIRE(graph.open(path.string()));
  REQUIRE(runSchemaFile(graph.handle(), ARGUS_TEST_LLM_SCHEMA));

  const RelayService service(RelayServiceDependencies{
      .conversation = std::make_shared<FakeConversation>(),
      .courier = std::make_shared<FakeCourier>(),
      .store = std::make_shared<GraphMessengerStore>(graph),
      .settings = {.enabled = true, .replyWindowSeconds = 300, .maxRelaysPerHour = 100000, .maxMessageChars = 500,
                   .maxVisitorNoticesPerHourPerCamera = 6, .retentionDays = 30}});

  constexpr int kRounds = 200;
  std::atomic<int> relaysSent{0};
  std::atomic<int> relaysFailed{0};
  std::atomic<int> writesDone{0};
  std::atomic<int> writesFailed{0};

  std::thread relays([&] {
    for (int round = 0; round < kRounds; ++round) {
      try {
        const auto result = service.relay({.senderId = 1,
                                           .senderName = "David Acme",
                                           .recipient = "Ana Lopez",
                                           .message = "mensaje " + std::to_string(round),
                                           .lang = "es",
                                           .afterQuestion = false,
                                           .dryRun = false,
                                           .roster = kRoster,
                                           .now = 1000 + round});
        if (result.threadId > 0)
          relaysSent.fetch_add(1);
        else
          relaysFailed.fetch_add(1);
      }
      catch (...) {
        relaysFailed.fetch_add(1);
      }
    }
  });

  std::thread memory([&] {
    for (int round = 0; round < kRounds; ++round) {
      const int64_t id = graph.createEntity(
          {.kind = "person", .canonical = "persona-" + std::to_string(round), .lang = "es", .personId = std::nullopt});
      if (id > 0)
        writesDone.fetch_add(1);
      else
        writesFailed.fetch_add(1);
    }
  });

  relays.join();
  memory.join();

  CHECK(relaysFailed.load() == 0);
  CHECK(writesFailed.load() == 0);
  CHECK(relaysSent.load() == kRounds);
  CHECK(writesDone.load() == kRounds);

  const std::scoped_lock lock(graph.mutex());
  CHECK(scalar(graph.handle(), "SELECT COUNT(*) FROM relay_thread") == kRounds);
  CHECK(scalar(graph.handle(), "SELECT COUNT(*) FROM memory_entity") == kRounds);
  CHECK(text(graph.handle(), "PRAGMA integrity_check") == "ok");
}
