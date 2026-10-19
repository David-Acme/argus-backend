#include <doctest/doctest.h>

#include <feature/conversation/services/conversation-context.hxx>

namespace
{
ConversationTurnRow turn(ConversationTurnRole role, std::string content)
{
  return {.id = 1, .userId = 7, .role = role, .leg = ConversationLegKind::Phone, .content = std::move(content),
          .createdAt = 1};
}
}

TEST_CASE("the system messages come first, then the tail, then the newest user message")
{
  const ConversationContext context = buildConversationContext(
      {.requestMessages = {{.role = "system", .content = "sistema A"},
                           {.role = "user", .content = "pregunta vieja"},
                           {.role = "assistant", .content = "respuesta vieja"},
                           {.role = "user", .content = "pregunta nueva"}},
       .tail = {turn(ConversationTurnRole::User, "pregunta t1"),
                turn(ConversationTurnRole::Assistant, "respuesta t1"),
                turn(ConversationTurnRole::Note, "la conversación continúa")}});
  REQUIRE(context.messages.size() == 5);
  CHECK(context.messages[0].role == "system");
  CHECK(context.messages[0].content == "sistema A");
  CHECK(context.messages[1].role == "user");
  CHECK(context.messages[1].content == "pregunta t1");
  CHECK(context.messages[2].role == "assistant");
  CHECK(context.messages[3].role == "system");
  CHECK(context.messages[3].content == "la conversación continúa");
  CHECK(context.messages[4].role == "user");
  CHECK(context.messages[4].content == "pregunta nueva");
  CHECK(context.latestUserText == "pregunta nueva");
}

TEST_CASE("the request's own history is replaced by the tail and never repeated")
{
  const ConversationContext context = buildConversationContext(
      {.requestMessages = {{.role = "user", .content = "vieja"}, {.role = "assistant", .content = "vieja r"},
                           {.role = "user", .content = "nueva"}},
       .tail = {turn(ConversationTurnRole::User, "vieja")}});
  CHECK(context.messages.size() == 2);
  CHECK(context.messages[0].content == "vieja");
  CHECK(context.messages[1].content == "nueva");
}

TEST_CASE("without a user message in the request the context is the system messages and the tail")
{
  const ConversationContext context = buildConversationContext(
      {.requestMessages = {{.role = "system", .content = "sistema"}},
       .tail = {turn(ConversationTurnRole::Assistant, "hola")}});
  REQUIRE(context.messages.size() == 2);
  CHECK(context.messages[1].role == "assistant");
  CHECK(context.latestUserText.empty());
}
