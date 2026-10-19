#include <doctest/doctest.h>

#include <feature/conversation/services/conversation-context.hxx>

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
constexpr int64_t kUser = 61;
constexpr int64_t kStart = 2000000;

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

ConversationServiceDeps depsOn(sqlite3* db, std::mutex& mutex)
{
  return {.db = db, .mutex = mutex, .policy = {.idleSeconds = 900}};
}
}

TEST_CASE("opening a conversation on another leg writes the switch note and keeps the earlier turns")
{
  Database database;
  std::mutex mutex;
  const ConversationService service(depsOn(database.handle, mutex));
  REQUIRE(service.recordTurn({.userId = kUser, .role = ConversationTurnRole::User,
                              .leg = ConversationLegKind::Phone, .content = "hola", .at = kStart}));
  REQUIRE(service.recordTurn({.userId = kUser, .role = ConversationTurnRole::Assistant,
                              .leg = ConversationLegKind::Phone, .content = "buenas", .at = kStart + 1}));

  const ConversationContext context = openConversation(
      service, {.userId = kUser,
                .leg = ConversationLegKind::Camera,
                .lang = "es",
                .at = kStart + 5,
                .tailTurns = 10,
                .requestMessages = {{.role = "system", .content = "S"},
                                    {.role = "user", .content = "y ahora?"}}});
  REQUIRE(context.messages.size() == 5);
  CHECK(context.messages[0].content == "S");
  CHECK(context.messages[1].content == "hola");
  CHECK(context.messages[2].content == "buenas");
  CHECK(context.messages[3].role == "system");
  CHECK(context.messages[3].content == "La conversación continúa desde la cámara; antes estaban en el teléfono.");
  CHECK(context.messages[4].content == "y ahora?");
  CHECK(context.latestUserText == "y ahora?");
}

TEST_CASE("a first conversation has no switch note and only the request's own messages")
{
  Database database;
  std::mutex mutex;
  const ConversationService service(depsOn(database.handle, mutex));
  const ConversationContext context = openConversation(
      service, {.userId = kUser,
                .leg = ConversationLegKind::Laptop,
                .lang = "es",
                .at = kStart,
                .tailTurns = 10,
                .requestMessages = {{.role = "user", .content = "primera"}}});
  REQUIRE(context.messages.size() == 1);
  CHECK(context.messages[0].content == "primera");
}

TEST_CASE("an exchange is stored as one user turn and one assistant turn on its leg")
{
  Database database;
  std::mutex mutex;
  const ConversationService service(depsOn(database.handle, mutex));
  CHECK(recordExchange(service, {.userId = kUser,
                                 .leg = ConversationLegKind::Camera,
                                 .userText = "y ahora?",
                                 .reply = "mira la puerta",
                                 .at = kStart}));
  const auto tail = service.contextTail(kUser, 10);
  REQUIRE(tail.size() == 2);
  CHECK(tail[0].role == ConversationTurnRole::User);
  CHECK(tail[0].content == "y ahora?");
  CHECK(tail[1].role == ConversationTurnRole::Assistant);
  CHECK(tail[1].leg == ConversationLegKind::Camera);
}

TEST_CASE("an exchange without a reply or without a user text stores nothing")
{
  Database database;
  std::mutex mutex;
  const ConversationService service(depsOn(database.handle, mutex));
  CHECK_FALSE(recordExchange(service, {.userId = kUser,
                                       .leg = ConversationLegKind::Phone,
                                       .userText = "hola",
                                       .reply = "",
                                       .at = kStart}));
  CHECK_FALSE(recordExchange(service, {.userId = kUser,
                                       .leg = ConversationLegKind::Phone,
                                       .userText = "",
                                       .reply = "buenas",
                                       .at = kStart}));
  CHECK(service.contextTail(kUser, 10).empty());
}
