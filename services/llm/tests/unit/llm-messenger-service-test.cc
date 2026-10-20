#include <doctest/doctest.h>

#include <errors/response-exception.hxx>
#include <feature/messenger/repositories/relay-thread/relay-thread-repository.hxx>
#include <feature/messenger/services/relay-service.hxx>
#include <feature/messenger/vocabulary/messenger-errors.hxx>
#include "support/direct-messenger-store.hxx"
#include "support/fake-messenger-ports.hxx"

#include <sqlite3.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

Database openSchema()
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
  Database db(raw, &sqlite3_close);
  std::ifstream in(ARGUS_TEST_LLM_SCHEMA);
  REQUIRE(in.good());
  std::stringstream sql;
  sql << in.rdbuf();
  char* error = nullptr;
  const int rc = sqlite3_exec(db.get(), sql.str().c_str(), nullptr, nullptr, &error);
  if (error != nullptr)
    sqlite3_free(error);
  REQUIRE(rc == SQLITE_OK);
  return db;
}

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

const std::vector<relay_recipient::Member>& roster()
{
  static const std::vector<relay_recipient::Member> members{
      {.userId = 1, .name = "David Acme"},
      {.userId = 2, .name = "Ana Lopez"},
      {.userId = 3, .name = "Ana Maria Ruiz"},
      {.userId = 4, .name = "Lucia"},
  };
  return members;
}

MessengerSettings settingsWith(int64_t relaysPerHour, int64_t visitorNotices = 6)
{
  return {.enabled = true,
          .replyWindowSeconds = 300,
          .maxRelaysPerHour = relaysPerHour,
          .maxMessageChars = 500,
          .maxVisitorNoticesPerHourPerCamera = visitorNotices,
          .retentionDays = 30};
}

struct World
{
  explicit World(MessengerSettings settings = settingsWith(20))
      : db(openSchema()),
        service(RelayServiceDependencies{.conversation = conversation,
                                         .courier = courier,
                                         .store = std::make_shared<DirectMessengerStore>(db.get()),
                                         .settings = settings})
  {
  }

  [[nodiscard]] int64_t threads() const { return scalar(db.get(), "SELECT COUNT(*) FROM relay_thread"); }

  Database db;
  std::shared_ptr<FakeConversation> conversation = std::make_shared<FakeConversation>();
  std::shared_ptr<FakeCourier> courier = std::make_shared<FakeCourier>();
  RelayService service;
};

RelayRequest relayFrom(std::string recipient, std::string message, bool afterQuestion = false, int64_t now = 1000)
{
  return {.senderId = 1,
          .senderName = "David Acme",
          .recipient = std::move(recipient),
          .message = std::move(message),
          .lang = "es",
          .afterQuestion = afterQuestion,
          .dryRun = false,
          .roster = roster(),
          .now = now};
}

ReplyRequest replyFrom(int64_t replier, std::string message, std::string recipient = {}, int64_t now = 1100)
{
  return {.replierId = replier,
          .replierName = replier == 2 ? "Ana Lopez" : "Lucia",
          .message = std::move(message),
          .lang = "es",
          .recipient = std::move(recipient),
          .afterQuestion = false,
          .dryRun = false,
          .roster = roster(),
          .now = now};
}

VisitorRequest visitorWith(std::string recipient, std::string need, int64_t now = 1000)
{
  return {.recipient = std::move(recipient),
          .need = std::move(need),
          .camera = "Sala",
          .lang = "es",
          .roster = roster(),
          .now = now};
}

}

TEST_CASE("a member speaking with Argus hears the message there and it lands in the conversation context")
{
  World world;
  world.courier->speaking.insert(2);
  const auto result = world.service.relay(relayFrom("Ana Lopez", "venga a comer"));
  CHECK(result.step == RelayStep::Sent);
  CHECK(result.outcome == RelayOutcome::Delivered);
  CHECK(result.channel == RelayChannel::Conversation);
  CHECK(result.recipientId == 2);
  REQUIRE(world.conversation->notes.size() == 1);
  CHECK(world.conversation->notes.front().userId == 2);
  CHECK(world.conversation->notes.front().text == "David Acme te manda decir: venga a comer");
  CHECK(world.threads() == 1);
}

TEST_CASE("a member who is not speaking with Argus gets the message left, and nothing is noted in a context")
{
  World world;
  const auto result = world.service.relay(relayFrom("Ana Lopez", "venga a comer"));
  CHECK(result.outcome == RelayOutcome::Notified);
  CHECK(result.channel == RelayChannel::Notification);
  CHECK(world.conversation->notes.empty());
  REQUIRE(world.courier->letters.size() == 1);
  const auto& letter = world.courier->letters.front();
  CHECK(letter.fromUserId == 1);
  CHECK(letter.toUserId == 2);
  CHECK(letter.toName == "Ana Lopez");
  CHECK(letter.kind == messenger::CourierKind::Relay);
  CHECK(letter.replyToKey.empty());
  CHECK(letter.expiresAt == 1000 + 30 * 24 * 3600);
}

TEST_CASE("only the sender's words travel: the body is the message and nothing else")
{
  World world;
  static_cast<void>(world.service.relay(relayFrom("Lucia", "trae pan")));
  REQUIRE(world.courier->letters.size() == 1);
  CHECK(world.courier->letters.front().body == "trae pan");
  CHECK(world.courier->letters.front().spokenLine == "David Acme te manda decir: trae pan");
  CHECK(world.courier->letters.front().title == "David Acme");
}

TEST_CASE("a dry run resolves the recipient and leaves no trace")
{
  World world;
  RelayRequest request = relayFrom("Lucia", "trae pan");
  request.dryRun = true;
  const auto planned = world.service.relay(request);
  CHECK(planned.step == RelayStep::Ready);
  CHECK(planned.recipientId == 4);
  CHECK(planned.recipientName == "Lucia");
  CHECK(world.courier->letters.empty());
  CHECK(world.threads() == 0);
}

TEST_CASE("an ambiguous or missing name asks once, and after the question it is no recipient")
{
  World world;
  const auto ambiguous = world.service.relay(relayFrom("Ana", "hola"));
  CHECK(ambiguous.step == RelayStep::AskRecipient);
  CHECK(ambiguous.candidates.size() == 2);
  const auto missing = world.service.relay(relayFrom("Pedro", "hola"));
  CHECK(missing.step == RelayStep::AskRecipient);
  CHECK(missing.candidates.empty());
  const auto after = world.service.relay(relayFrom("Pedro", "hola", true));
  CHECK(after.step == RelayStep::NoRecipient);
  CHECK(world.courier->letters.empty());
}

TEST_CASE("a message to oneself, an empty one, a long one or a disabled messenger is refused")
{
  World world;
  CHECK_THROWS_AS(static_cast<void>(world.service.relay(relayFrom("David Acme", "hola"))), ResponseException);
  CHECK_THROWS_AS(static_cast<void>(world.service.relay(relayFrom("Lucia", "   "))), ResponseException);
  CHECK_THROWS_AS(static_cast<void>(world.service.relay(relayFrom("Lucia", std::string(501, 'a')))), ResponseException);
  MessengerSettings off = settingsWith(20);
  off.enabled = false;
  World disabled(off);
  CHECK_THROWS_AS(static_cast<void>(disabled.service.relay(relayFrom("Lucia", "hola"))), ResponseException);
  CHECK(world.courier->letters.empty());
}

TEST_CASE("the hourly cap refuses the next relay and a dry run counts against it too")
{
  World world(settingsWith(2));
  static_cast<void>(world.service.relay(relayFrom("Lucia", "uno", false, 1000)));
  static_cast<void>(world.service.relay(relayFrom("Lucia", "dos", false, 1001)));
  RelayRequest third = relayFrom("Lucia", "tres", false, 1002);
  third.dryRun = true;
  CHECK_THROWS_AS(static_cast<void>(world.service.relay(third)), ResponseException);
  CHECK(world.courier->letters.size() == 2);
  const auto later = world.service.relay(relayFrom("Lucia", "otra hora", false, 1000 + 3601));
  CHECK(later.step == RelayStep::Sent);
}

TEST_CASE("a message nobody can carry is refused and leaves no open thread")
{
  World world;
  world.courier->accept = false;
  CHECK_THROWS_AS(static_cast<void>(world.service.relay(relayFrom("Lucia", "hola"))), ResponseException);
  CHECK(world.threads() == 0);
}

TEST_CASE("an answer with exactly one open thread goes back to its sender and names the message it answers")
{
  World world;
  static_cast<void>(world.service.relay(relayFrom("Ana Lopez", "venga a comer")));
  const std::string relayed = world.courier->letters.front().commandId;
  const auto answer = world.service.reply(replyFrom(2, "ya voy"));
  CHECK(answer.step == RelayStep::Sent);
  REQUIRE(world.courier->letters.size() == 2);
  const auto& back = world.courier->letters.back();
  CHECK(back.toUserId == 1);
  CHECK(back.kind == messenger::CourierKind::Reply);
  CHECK(back.replyToKey == relayed);
  CHECK(scalar(world.db.get(), "SELECT COUNT(*) FROM relay_thread WHERE state = 'answered'") == 1);
}

TEST_CASE("an answer with open threads from two senders asks who, and with none asks who too")
{
  World world;
  static_cast<void>(world.service.relay(relayFrom("Ana Lopez", "venga a comer")));
  RelayRequest fromLucia = relayFrom("Ana Lopez", "trae pan");
  fromLucia.senderId = 4;
  fromLucia.senderName = "Lucia";
  static_cast<void>(world.service.relay(fromLucia));
  const auto who = world.service.reply(replyFrom(2, "ya voy"));
  CHECK(who.step == RelayStep::AskRecipient);
  CHECK(who.candidates.size() == 2);
  const auto nobody = world.service.reply(replyFrom(4, "ok"));
  CHECK(nobody.step == RelayStep::AskRecipient);
  CHECK(nobody.candidates.empty());
}

TEST_CASE("the sweep expires unanswered threads and marks each message expired in the inbox")
{
  World world;
  static_cast<void>(world.service.relay(relayFrom("Lucia", "llámame", false, 1000)));
  const std::string relayed = world.courier->letters.front().commandId;
  const auto swept = world.service.sweep(1000 + 301);
  CHECK(swept.threadsExpired == 1);
  CHECK(swept.settled == 1);
  REQUIRE(world.courier->settled.size() == 1);
  CHECK(world.courier->settled.front().messageKey == relayed);
  CHECK(world.courier->settled.front().settlement == messenger::CourierSettlement::Expired);
  const auto again = world.service.sweep(1000 + 302);
  CHECK(again.threadsExpired == 0);
}

TEST_CASE("a visitor learns nothing: the same line whether the name matched or not")
{
  World world;
  const auto matched = world.service.visitorAsk(visitorWith("Lucia", "traigo un paquete"));
  const auto unmatched = world.service.visitorAsk(visitorWith("Pedro", "traigo un paquete"));
  CHECK(matched.line == unmatched.line);
  CHECK_FALSE(matched.needsAnswer);
  REQUIRE(world.courier->letters.size() == 1);
  const auto& letter = world.courier->letters.front();
  CHECK(letter.kind == messenger::CourierKind::Visitor);
  CHECK(letter.fromUserId == 0);
  CHECK(letter.toUserId == 4);
  CHECK(letter.body == "traigo un paquete");
}

TEST_CASE("a visitor's notices stop at the camera's hourly limit, and the visitor still hears the same line")
{
  World world(settingsWith(20, 1));
  const auto first = world.service.visitorAsk(visitorWith("Lucia", "hola", 1000));
  const auto second = world.service.visitorAsk(visitorWith("Lucia", "hola otra vez", 1001));
  CHECK(first.line == second.line);
  CHECK(world.courier->letters.size() == 1);
}
