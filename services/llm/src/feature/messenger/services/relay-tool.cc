#include "relay-tool.hxx"

#include <errors/response-exception.hxx>
#include <feature/messenger/services/relay-lines.hxx>
#include <mcp/schema.hxx>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace schema = argus::mcp::schema;

constexpr std::string_view kCapability = "messages.relay";
constexpr std::string_view kAskCode = "ask_recipient";
constexpr std::string_view kNoRecipientCode = "no_recipient";
constexpr std::string_view kUnknownSenderCode = "sender_unknown";
constexpr std::string_view kConsentCode = "confirmation_invalid";
constexpr std::string_view kConfirmation = "confirmation";

int64_t nowSeconds(const tools::ToolContext& context)
{
  if (context.now)
    return *context.now;
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string nameOf(const std::vector<relay_recipient::Member>& roster, int64_t userId)
{
  for (const auto& member : roster)
    if (member.userId == userId)
      return member.name;
  return {};
}

tools::ToolResult refused(std::string_view tool, std::string output, std::string code, Json::Value data = Json::Value(Json::objectValue))
{
  return {.tool = std::string(tool), .ok = false, .output = std::move(output), .data = std::move(data), .code = std::move(code)};
}

Json::Value namesJson(const std::vector<std::string>& names)
{
  Json::Value list(Json::arrayValue);
  for (const auto& name : names)
    list.append(name);
  Json::Value data(Json::objectValue);
  data["candidates"] = std::move(list);
  return data;
}

struct RelayCall
{
  const RelayService& service;
  const HouseholdRoster& roster;
  argus::mcp::ConfirmationLedger& ledger;
  const tools::ToolCall& call;
};

struct RelayAttempt
{
  int64_t senderId{0};
  std::string senderName;
  std::string recipient;
  std::string message;
  std::string lang;
  bool reply{false};
  bool afterQuestion{false};
  bool dryRun{false};
  std::vector<relay_recipient::Member> roster;
  int64_t now{0};
};

RelayResult attempt(const RelayService& service, const RelayAttempt& input)
{
  if (input.reply)
    return service.reply({.replierId = input.senderId,
                          .replierName = input.senderName,
                          .message = input.message,
                          .lang = input.lang,
                          .recipient = input.recipient,
                          .afterQuestion = input.afterQuestion,
                          .dryRun = input.dryRun,
                          .roster = input.roster,
                          .now = input.now});
  return service.relay({.senderId = input.senderId,
                        .senderName = input.senderName,
                        .recipient = input.recipient,
                        .message = input.message,
                        .lang = input.lang,
                        .afterQuestion = input.afterQuestion,
                        .dryRun = input.dryRun,
                        .roster = input.roster,
                        .now = input.now});
}

argus::mcp::ConfirmationKey consentKey(const tools::ToolCall& call, const RelayResult& planned, const RelayAttempt& input)
{
  return {.userId = call.context.userId,
          .tool = std::string(kRelayToolName),
          .target = std::to_string(planned.recipientId) + (input.reply ? "|reply|" : "|relay|") + input.message};
}

tools::ToolResult unresolved(const RelayResult& result, std::string_view lang)
{
  if (result.step == RelayStep::AskRecipient)
    return refused(kRelayToolName, relay_lines::recipientQuestion(result.candidates, lang), std::string(kAskCode),
                   namesJson(result.candidates));
  return refused(kRelayToolName, std::string(relay_lines::outcome(RelayOutcome::NoRecipient, lang)), std::string(kNoRecipientCode));
}

tools::ToolResult handle(const RelayCall& relay)
{
  const tools::ToolCall& call = relay.call;
  const std::string lang = call.context.lang;
  const auto members = relay.roster.members();
  const std::string senderName = nameOf(members, call.context.userId);
  if (senderName.empty())
    return refused(kRelayToolName, std::string(relay_lines::outcome(RelayOutcome::NoRecipient, lang)), std::string(kUnknownSenderCode));
  RelayAttempt input{.senderId = call.context.userId,
                     .senderName = senderName,
                     .recipient = call.arguments.get("recipient", "").asString(),
                     .message = call.arguments.get("message", "").asString(),
                     .lang = lang,
                     .reply = call.arguments.get("reply", false).asBool(),
                     .afterQuestion = call.arguments.get("afterQuestion", false).asBool(),
                     .dryRun = true,
                     .roster = members,
                     .now = nowSeconds(call.context)};
  const std::string token = call.arguments.get(std::string(kConfirmation), "").asString();
  try {
    const RelayResult planned = attempt(relay.service, input);
    if (planned.step != RelayStep::Ready)
      return unresolved(planned, lang);
    const argus::mcp::ConfirmationKey key = consentKey(call, planned, input);
    if (token.empty()) {
      Json::Value data(Json::objectValue);
      data["needsConfirmation"] = true;
      data["confirmation"] = relay.ledger.issue(key);
      data["recipient"] = planned.recipientName;
      return {.tool = std::string(kRelayToolName),
              .ok = true,
              .output = relay_lines::consentQuestion(
                  {.recipient = planned.recipientName, .message = input.message, .lang = lang}),
              .data = std::move(data),
              .code = {}};
    }
    if (!relay.ledger.consume(key, token))
      return refused(kRelayToolName, std::string(relay_lines::consentExpired(lang)), std::string(kConsentCode));
    input.dryRun = false;
    const RelayResult sent = attempt(relay.service, input);
    if (sent.step != RelayStep::Sent)
      return unresolved(sent, lang);
    Json::Value data(Json::objectValue);
    data["outcome"] = std::string(relayOutcomeToString(sent.outcome));
    return {.tool = std::string(kRelayToolName),
            .ok = true,
            .output = std::string(relay_lines::outcome(sent.outcome, lang)),
            .data = std::move(data),
            .code = {}};
  }
  catch (const ResponseException& error) {
    return refused(kRelayToolName, error.what(), error.errorCode());
  }
}

}

argus::mcp::ToolSpec relayToolSpec()
{
  return {.name = std::string(kRelayToolName),
          .title = "Mensaje a un miembro de la casa",
          .description = "Lleva un mensaje de quien habla a otro miembro de la casa, con sus propias palabras.",
          .inputSchema = schema::object({{.name = "recipient", .schema = schema::text(), .required = false},
                                         {.name = "message", .schema = schema::text(), .required = false},
                                         {.name = "reply", .schema = schema::boolean(), .required = false},
                                         {.name = "afterQuestion", .schema = schema::boolean(), .required = false},
                                         {.name = std::string(kConfirmation), .schema = schema::text(), .required = false}}),
          .annotations = {.readOnly = false, .destructive = true, .idempotent = false, .openWorld = false},
          .module = {},
          .capability = std::string(kCapability)};
}

tools::ToolDescriptor relayTool(RelayToolInput input)
{
  return {.spec = relayToolSpec(),
          .handler = [service = std::move(input.service), roster = std::move(input.roster),
                      ledger = std::move(input.ledger)](const tools::ToolCall& call) {
            return handle({.service = *service, .roster = *roster, .ledger = *ledger, .call = call});
          }};
}
