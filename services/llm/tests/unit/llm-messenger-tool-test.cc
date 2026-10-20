#include <doctest/doctest.h>

#include <feature/llm/services/turn/deciders.hxx>
#include <feature/messenger/services/relay-tool.hxx>
#include <feature/messenger/vocabulary/messenger-errors.hxx>
#include "support/direct-messenger-store.hxx"
#include "support/fake-messenger-ports.hxx"

#include <sqlite3.h>

#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <test-support/value-of.hxx>

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

class FakeRoster final : public HouseholdRoster
{
public:
  [[nodiscard]] std::vector<relay_recipient::Member> members() const override
  {
    return {{.userId = 1, .name = "David Acme"},
            {.userId = 2, .name = "Ana Lopez"},
            {.userId = 3, .name = "Ana Maria Ruiz"}};
  }
};

tools::ToolCall callAs(int64_t userId, const Json::Value& arguments, int64_t now = 1000)
{
  tools::ToolCall call;
  call.name = std::string(kRelayToolName);
  call.arguments = arguments;
  call.context.userId = userId;
  call.context.lang = "es";
  call.context.now = now;
  return call;
}

struct Harness
{
  Harness()
      : db(openSchema()),
        courier(std::make_shared<FakeCourier>()),
        service(std::make_shared<const RelayService>(RelayServiceDependencies{
            .conversation = std::make_shared<FakeConversation>(),
            .courier = courier,
            .store = std::make_shared<DirectMessengerStore>(db.get()),
            .settings = MessengerSettings{}})),
        roster(std::make_shared<const FakeRoster>()),
        descriptor(relayTool({.service = service,
                              .roster = roster,
                              .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()}))
  {
  }

  [[nodiscard]] tools::ToolResult confirmed(int64_t userId, Json::Value arguments, int64_t now = 1000) const
  {
    auto preview = descriptor.handler(callAs(userId, arguments, now));
    if (!preview.ok || !preview.data["needsConfirmation"].asBool())
      return preview;
    arguments["confirmation"] = preview.data["confirmation"];
    return descriptor.handler(callAs(userId, arguments, now));
  }

  Database db;
  std::shared_ptr<FakeCourier> courier;
  std::shared_ptr<const RelayService> service;
  std::shared_ptr<const HouseholdRoster> roster;
  tools::ToolDescriptor descriptor;
};

struct RelayArguments
{
  std::string recipient;
  std::string message;
  bool reply{false};
};

Json::Value argumentsOf(const RelayArguments& relay)
{
  Json::Value arguments(Json::objectValue);
  arguments["recipient"] = relay.recipient;
  arguments["message"] = relay.message;
  arguments["reply"] = relay.reply;
  return arguments;
}

}

TEST_CASE("the relay tool is a core tool gated by messages.relay that needs the sender's yes")
{
  const auto spec = relayToolSpec();
  CHECK(spec.name == "message.relay");
  CHECK(spec.capability == "messages.relay");
  CHECK(spec.module.empty());
  CHECK(spec.annotations.destructive);
  CHECK(spec.inputSchema["properties"].isMember("confirmation"));
  CHECK_FALSE(spec.annotations.openWorld);
}

TEST_CASE("a relay first reads the message back and sends nothing until the sender confirms it")
{
  Harness harness;
  const auto preview = harness.descriptor.handler(callAs(1, argumentsOf({.recipient = "Ana Lopez", .message = "venga a comer", .reply = false})));
  CHECK(preview.ok);
  CHECK(preview.data["needsConfirmation"].asBool());
  CHECK_FALSE(preview.data["confirmation"].asString().empty());
  CHECK(preview.output == "¿Le digo a Ana Lopez: «venga a comer»?");
  CHECK(harness.courier->letters.empty());

  Json::Value forged = argumentsOf({.recipient = "Ana Lopez", .message = "venga a comer", .reply = false});
  forged["confirmation"] = "not-the-code";
  const auto refused = harness.descriptor.handler(callAs(1, forged));
  CHECK_FALSE(refused.ok);
  CHECK(refused.code == "confirmation_invalid");
  CHECK(harness.courier->letters.empty());
}

TEST_CASE("the code of one message never sends another")
{
  Harness harness;
  const auto preview = harness.descriptor.handler(callAs(1, argumentsOf({.recipient = "Ana Lopez", .message = "venga a comer", .reply = false})));
  Json::Value swapped = argumentsOf({.recipient = "Ana Lopez", .message = "no vengas", .reply = false});
  swapped["confirmation"] = preview.data["confirmation"];
  const auto refused = harness.descriptor.handler(callAs(1, swapped));
  CHECK_FALSE(refused.ok);
  CHECK(harness.courier->letters.empty());
}

TEST_CASE("a confirmed relay is carried and the sender hears what happened, without the recipient's whereabouts")
{
  Harness harness;
  const auto result = harness.confirmed(1, argumentsOf({.recipient = "Ana Lopez", .message = "venga a comer", .reply = false}));
  CHECK(result.ok);
  CHECK(result.output == "Le dejé el mensaje.");
  CHECK(result.data["outcome"].asString() == "notified");
  REQUIRE(harness.courier->letters.size() == 1);
  CHECK(harness.courier->letters.front().toUserId == 2);
  CHECK(harness.courier->letters.front().body == "venga a comer");
}

TEST_CASE("an ambiguous name returns the one question and does not send")
{
  Harness harness;
  const auto result = harness.descriptor.handler(callAs(1, argumentsOf({.recipient = "Ana", .message = "venga a comer", .reply = false})));
  CHECK_FALSE(result.ok);
  CHECK(result.code == "ask_recipient");
  CHECK(result.output == "¿Se lo digo a Ana Lopez o Ana Maria Ruiz?");
  REQUIRE(result.data["candidates"].size() == 2);
  CHECK(result.data["candidates"][0].asString() == "Ana Lopez");
  CHECK(result.data["candidates"][1].asString() == "Ana Maria Ruiz");
  CHECK(harness.courier->letters.empty());
}

TEST_CASE("a refused relay returns its wire code and its reason, never throws")
{
  Harness harness;
  const auto empty = harness.descriptor.handler(callAs(1, argumentsOf({.recipient = "Ana Lopez", .message = "   ", .reply = false})));
  CHECK_FALSE(empty.ok);
  CHECK(empty.code == std::string(MessengerErrors::MessageRequired.wireCode()));
  CHECK(empty.output == std::string(MessengerErrors::MessageRequired.message));
  const auto self = harness.descriptor.handler(callAs(1, argumentsOf({.recipient = "David", .message = "hola", .reply = false})));
  CHECK_FALSE(self.ok);
  CHECK(self.code == std::string(MessengerErrors::SelfRecipient.wireCode()));
}

TEST_CASE("an answer travels back on the open thread through the same tool")
{
  Harness harness;
  static_cast<void>(harness.confirmed(1, argumentsOf({.recipient = "Ana Lopez", .message = "venga a comer", .reply = false}), 1000));
  const auto answer = harness.confirmed(2, argumentsOf({.recipient = "", .message = "ya voy", .reply = true}), 1100);
  CHECK(answer.ok);
  REQUIRE(harness.courier->letters.size() == 2);
  CHECK(harness.courier->letters.back().toUserId == 1);
  CHECK(harness.courier->letters.back().body == "ya voy");
  CHECK(harness.courier->letters.back().kind == messenger::CourierKind::Reply);
}

TEST_CASE("a caller the roster does not know is refused as an unknown sender")
{
  Harness harness;
  const auto result = harness.descriptor.handler(callAs(99, argumentsOf({.recipient = "Ana Lopez", .message = "hola", .reply = false})));
  CHECK_FALSE(result.ok);
  CHECK(result.code == "sender_unknown");
  CHECK(harness.courier->letters.empty());
}

TEST_CASE("the rules tier picks the relay only when the tool is offered, and an answer carries the reply flag")
{
  Harness harness;
  const tools::ToolHandle handle = std::make_shared<const tools::ToolDescriptor>(harness.descriptor);
  const std::vector<tools::ToolHandle> offered{handle};
  const std::vector<tools::ToolHandle> none;
  const ModuleSnapshot modules;
  const turn::RuleDecider decider;

  const auto picked = valueOf(decider.decide({.utterance = "Argus, llama a Ana Lopez y dile que venga a comer",
                                      .lang = "es",
                                      .offered = offered,
                                      .modules = modules,
                                      .previousAssistant = {}}));
  CHECK(picked.tool == "message.relay");
  CHECK(picked.arguments.get("recipient", "").asString() == "ana lopez");
  CHECK(picked.arguments.get("message", "").asString() == "venga a comer");
  CHECK_FALSE(picked.arguments.get("reply", true).asBool());

  const auto answer = valueOf(decider.decide({.utterance = "dile que ya voy",
                                      .lang = "es",
                                      .offered = offered,
                                      .modules = modules,
                                      .previousAssistant = {}}));
  CHECK(answer.arguments.get("reply", false).asBool());

  CHECK_FALSE(decider
                  .decide({.utterance = "llama a Ana Lopez y dile que venga a comer",
                           .lang = "es",
                           .offered = none,
                           .modules = modules,
                           .previousAssistant = {}})
                  .has_value());
}
