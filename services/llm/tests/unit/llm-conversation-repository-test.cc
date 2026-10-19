#include <doctest/doctest.h>

#include <feature/conversation/repositories/conversation/conversation-repository.hxx>

#include <sqlite3.h>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif

namespace
{
constexpr int64_t kUser = 7;
constexpr int64_t kOther = 8;

struct Database
{
  sqlite3* handle{nullptr};

  Database()
  {
    sqlite3_open(":memory:", &handle);
    sqlite3_exec(handle, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    std::ifstream file(ARGUS_TEST_MEMORY_SCHEMA);
    const std::string schema((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    sqlite3_exec(handle, schema.c_str(), nullptr, nullptr, nullptr);
  }

  ~Database() { sqlite3_close(handle); }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
};

ConversationTurnAppendInput turn(int64_t user, ConversationTurnRole role, ConversationLegKind leg,
                                 std::string content, int64_t at)
{
  return {.userId = user, .role = role, .leg = leg, .content = std::move(content), .at = at};
}
}

TEST_CASE("a leg touch creates the conversation and a second touch switches its leg")
{
  Database database;
  const ConversationRepository repository;
  REQUIRE(repository.touchLeg(database.handle,
                              {.userId = kUser, .leg = ConversationLegKind::Camera, .at = 100}));
  auto row = repository.find(database.handle, kUser);
  REQUIRE(row.has_value());
  CHECK(row->activeLeg == ConversationLegKind::Camera);
  CHECK(row->createdAt == 100);

  REQUIRE(repository.touchLeg(database.handle,
                              {.userId = kUser, .leg = ConversationLegKind::Phone, .at = 200}));
  row = repository.find(database.handle, kUser);
  REQUIRE(row.has_value());
  CHECK(row->activeLeg == ConversationLegKind::Phone);
  CHECK(row->createdAt == 100);
  CHECK(row->updatedAt == 200);
}

TEST_CASE("turns are returned in arrival order and the limit keeps the newest")
{
  Database database;
  const ConversationRepository repository;
  REQUIRE(repository.touchLeg(database.handle,
                              {.userId = kUser, .leg = ConversationLegKind::Phone, .at = 1}));
  REQUIRE(repository.appendTurn(database.handle, turn(kUser, ConversationTurnRole::User,
                                                      ConversationLegKind::Phone, "uno", 10)));
  REQUIRE(repository.appendTurn(database.handle, turn(kUser, ConversationTurnRole::Assistant,
                                                      ConversationLegKind::Phone, "dos", 20)));
  REQUIRE(repository.appendTurn(database.handle, turn(kUser, ConversationTurnRole::Note,
                                                      ConversationLegKind::Camera, "tres", 30)));
  const auto turns = repository.recentTurns(database.handle, {.userId = kUser, .limit = 2});
  REQUIRE(turns.size() == 2);
  CHECK(turns[0].content == "dos");
  CHECK(turns[0].role == ConversationTurnRole::Assistant);
  CHECK(turns[1].content == "tres");
  CHECK(turns[1].leg == ConversationLegKind::Camera);
  const auto row = repository.find(database.handle, kUser);
  REQUIRE(row.has_value());
  CHECK(row->lastTurnAt == 30);
}

TEST_CASE("a turn cannot be stored for a conversation that does not exist")
{
  Database database;
  const ConversationRepository repository;
  CHECK_FALSE(repository.appendTurn(database.handle, turn(kOther, ConversationTurnRole::User,
                                                          ConversationLegKind::Phone, "huerfano", 5)));
  CHECK(repository.recentTurns(database.handle, {.userId = kOther, .limit = 5}).empty());
}

TEST_CASE("the summary is stored with its turn count")
{
  Database database;
  const ConversationRepository repository;
  REQUIRE(repository.touchLeg(database.handle,
                              {.userId = kUser, .leg = ConversationLegKind::Laptop, .at = 1}));
  REQUIRE(repository.setSummary(database.handle, {.userId = kUser,
                                                  .summary = "resumen corto",
                                                  .summaryTurns = 12,
                                                  .at = 50}));
  const auto row = repository.find(database.handle, kUser);
  REQUIRE(row.has_value());
  CHECK(row->summary == "resumen corto");
  CHECK(row->summaryTurns == 12);
}

TEST_CASE("the retention purge removes only the turns older than the cutoff")
{
  Database database;
  const ConversationRepository repository;
  REQUIRE(repository.touchLeg(database.handle,
                              {.userId = kUser, .leg = ConversationLegKind::Phone, .at = 1}));
  REQUIRE(repository.appendTurn(database.handle, turn(kUser, ConversationTurnRole::User,
                                                      ConversationLegKind::Phone, "viejo", 10)));
  REQUIRE(repository.appendTurn(database.handle, turn(kUser, ConversationTurnRole::User,
                                                      ConversationLegKind::Phone, "nuevo", 100)));
  CHECK(repository.purgeTurnsBefore(database.handle, 50) == 1);
  const auto turns = repository.recentTurns(database.handle, {.userId = kUser, .limit = 10});
  REQUIRE(turns.size() == 1);
  CHECK(turns.front().content == "nuevo");
}
