#include <doctest/doctest.h>

#include <feature/conversation/services/conversation-service.hxx>

#include <sqlite3.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif

namespace
{
constexpr int64_t kUser = 21;
constexpr int64_t kOther = 22;
constexpr int64_t kStart = 1000000;

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

ConversationService serviceOn(sqlite3* db, std::mutex& mutex)
{
  return ConversationService({.db = db, .mutex = mutex, .policy = {.idleSeconds = 900}});
}
}

TEST_CASE("a recorded turn persists and makes its leg the active one")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Camera,
                              .content = "hola",
                              .at = kStart}));
  CHECK(service.hasOpenConversation({.userId = kUser, .now = kStart + 10}));
  const auto tail = service.contextTail(kUser, 5);
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().content == "hola");
  CHECK(tail.front().leg == ConversationLegKind::Camera);
}

TEST_CASE("the state survives a new service instance on the same database")
{
  Database database;
  std::mutex mutex;
  REQUIRE(serviceOn(database.handle, mutex).recordTurn({.userId = kUser,
                                                        .role = ConversationTurnRole::Assistant,
                                                        .leg = ConversationLegKind::Laptop,
                                                        .content = "listo",
                                                        .at = kStart}));
  const auto reopened = serviceOn(database.handle, mutex);
  const auto tail = reopened.contextTail(kUser, 5);
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().role == ConversationTurnRole::Assistant);
  CHECK(tail.front().leg == ConversationLegKind::Laptop);
}

TEST_CASE("an allowed takeover switches the leg and writes the switch note into the tail")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Camera,
                              .content = "¿qué hora es?",
                              .at = kStart}));
  const TakeoverResult taken = service.takeover({.userId = kUser,
                                                 .candidate = ConversationLegKind::Phone,
                                                 .allowed = true,
                                                 .lang = "es",
                                                 .cameraPlaceWithArticle = "la sala",
                                                 .at = kStart + 5});
  CHECK(taken.switched);
  CHECK(taken.note == "La conversación continúa desde el teléfono; antes estaban en la cámara de la sala.");
  const auto tail = service.contextTail(kUser, 5);
  REQUIRE(tail.size() == 2);
  CHECK(tail.back().role == ConversationTurnRole::Note);
  CHECK(tail.back().leg == ConversationLegKind::Phone);
  CHECK(tail.back().content == taken.note);
}

TEST_CASE("a refused takeover changes nothing and writes no note")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone,
                              .content = "hola",
                              .at = kStart}));
  const TakeoverResult refused = service.takeover({.userId = kUser,
                                                   .candidate = ConversationLegKind::Camera,
                                                   .allowed = false,
                                                   .lang = "es",
                                                   .cameraPlaceWithArticle = "la sala",
                                                   .at = kStart + 5});
  CHECK_FALSE(refused.switched);
  CHECK(refused.note.empty());
  CHECK(service.contextTail(kUser, 5).size() == 1);
}

TEST_CASE("a takeover with no earlier conversation sets the leg without a note")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  const TakeoverResult first = service.takeover({.userId = kUser,
                                                 .candidate = ConversationLegKind::Phone,
                                                 .allowed = true,
                                                 .lang = "es",
                                                 .cameraPlaceWithArticle = "la sala",
                                                 .at = kStart});
  CHECK_FALSE(first.switched);
  CHECK(service.contextTail(kUser, 5).empty());
  CHECK_FALSE(service.hasOpenConversation({.userId = kUser, .now = kStart}));
}

TEST_CASE("a conversation is open within the idle window and idle after it")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone,
                              .content = "hola",
                              .at = kStart}));
  CHECK(service.hasOpenConversation({.userId = kUser, .now = kStart + 900}));
  CHECK_FALSE(service.hasOpenConversation({.userId = kUser, .now = kStart + 901}));
  CHECK(service.idleUsers(kStart + 900).empty());
  CHECK(service.idleUsers(kStart + 901) == std::vector<int64_t>{kUser});
}

TEST_CASE("an inbound message lands in an open conversation only")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  CHECK_FALSE(service.appendInbound({.userId = kOther, .content = "come", .at = kStart}));
  CHECK(service.contextTail(kOther, 5).empty());

  REQUIRE(service.recordTurn({.userId = kOther,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Laptop,
                              .content = "hola",
                              .at = kStart}));
  REQUIRE(service.appendInbound({.userId = kOther, .content = "Ana te manda decir que vengas a comer", .at = kStart + 1}));
  const auto tail = service.contextTail(kOther, 5);
  REQUIRE(tail.size() == 2);
  CHECK(tail.back().role == ConversationTurnRole::Note);
  CHECK(tail.back().leg == ConversationLegKind::Laptop);
}

TEST_CASE("the retention purge removes the turns older than the retention window")
{
  Database database;
  std::mutex mutex;
  const auto service = serviceOn(database.handle, mutex);
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone,
                              .content = "viejo",
                              .at = kStart}));
  REQUIRE(service.recordTurn({.userId = kUser,
                              .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone,
                              .content = "nuevo",
                              .at = kStart + 40 * 86400}));
  CHECK(service.purgeExpired(kStart + 40 * 86400, 30) == 1);
  const auto tail = service.contextTail(kUser, 5);
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().content == "nuevo");
}
