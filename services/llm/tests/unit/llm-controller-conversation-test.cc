#include <doctest/doctest.h>

#include <errors/response-exception.hxx>
#include <feature/conversation/services/conversation-store.hxx>
#include <feature/conversation/services/conversation-transcript.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <llm/llm-speech.hxx>
#include <shared/vocabulary/utterance-screen.hxx>

#include <memory>
#include <string>
#include <vector>

namespace
{
class FakeStore final : public ConversationStore
{
public:
  [[nodiscard]] bool recordTurn(const TurnRecord& turn) const override
  {
    records.push_back(turn);
    return true;
  }

  [[nodiscard]] TakeoverResult takeover(const TakeoverRequest& request) const override
  {
    takeovers.push_back(request);
    return {.switched = false, .note = {}};
  }

  [[nodiscard]] std::vector<ConversationTurnRow> contextTail(int64_t, int64_t) const override
  {
    ++tails;
    return {};
  }

  [[nodiscard]] bool untouched() const { return records.empty() && takeovers.empty() && tails == 0; }

  mutable std::vector<TurnRecord> records;
  mutable std::vector<TakeoverRequest> takeovers;
  mutable int tails{0};
};

class FakeTranscript final : public ConversationTranscript
{
public:
  void record(const TranscriptExchange& exchange) const override { exchanges.push_back(exchange); }

  mutable std::vector<TranscriptExchange> exchanges;
};

class WithholdingScreen final : public turn::UtteranceScreen
{
public:
  [[nodiscard]] turn::ScreenedText screen(const turn::ScreenedUtterance&) const override
  {
    return {.text = {}, .withheld = true};
  }
};

class RewritingScreen final : public turn::UtteranceScreen
{
public:
  [[nodiscard]] turn::ScreenedText screen(const turn::ScreenedUtterance&) const override
  {
    return {.text = "mi clave es [omitida]", .withheld = false};
  }
};

ChatRequest chatFrom(bool useConversation)
{
  ChatRequest request;
  request.userId = 7;
  request.role = UserRole::Resident;
  request.lang = "es";
  request.messages = {{.role = "system", .content = "S"}, {.role = "user", .content = "hola"}};
  request.useConversation = useConversation;
  request.conversationLeg = ConversationLegKind::Phone;
  return request;
}

LlmChatOutcome answer(const ChatRequest&)
{
  LlmChatOutcome outcome;
  outcome.text = "buenas";
  return outcome;
}
}

TEST_CASE("a turn with use_conversation opens the store and records the exchange on its leg")
{
  LlmController controller;
  const auto store = std::make_shared<FakeStore>();
  controller.setConversation(store);
  controller.setTurnEngine(answer);

  const LlmChatOutcome outcome = controller.chatSync(chatFrom(true));
  CHECK(outcome.text == "buenas");
  REQUIRE(store->takeovers.size() == 1);
  CHECK(store->takeovers.front().candidate == ConversationLegKind::Phone);
  CHECK(store->tails == 1);
  REQUIRE(store->records.size() == 2);
  CHECK(store->records[0].role == ConversationTurnRole::User);
  CHECK(store->records[0].content == "hola");
  CHECK(store->records[0].leg == ConversationLegKind::Phone);
  CHECK(store->records[1].role == ConversationTurnRole::Assistant);
  CHECK(store->records[1].content == "buenas");
}

TEST_CASE("a turn without use_conversation never touches the store")
{
  LlmController controller;
  const auto store = std::make_shared<FakeStore>();
  controller.setConversation(store);
  controller.setTurnEngine(answer);

  const LlmChatOutcome outcome = controller.chatSync(chatFrom(false));
  CHECK(outcome.text == "buenas");
  CHECK(store->untouched());
}

TEST_CASE("a turn withheld by duress records no switch note and never changes the leg")
{
  LlmController controller;
  const auto store = std::make_shared<FakeStore>();
  controller.setConversation(store);
  controller.setTurnEngine(answer);
  const WithholdingScreen screen;
  controller.useScreen(screen);

  const LlmChatOutcome outcome = controller.chatSync(chatFrom(true));
  CHECK(outcome.speech == std::string(llm_speech::kUnheard));
  CHECK(outcome.text.empty());
  CHECK(store->untouched());
}

TEST_CASE("the conversation records the screened text, never the raw text the screen rewrote")
{
  LlmController controller;
  const auto store = std::make_shared<FakeStore>();
  controller.setConversation(store);
  std::vector<ChatMessage> modelSaw;
  controller.setTurnEngine([&modelSaw](const ChatRequest& request) {
    modelSaw = request.messages;
    LlmChatOutcome outcome;
    outcome.text = "listo";
    return outcome;
  });
  const RewritingScreen screen;
  controller.useScreen(screen);

  ChatRequest request = chatFrom(true);
  request.messages = {{.role = "user", .content = "mi clave es 4421"}};
  const LlmChatOutcome outcome = controller.chatSync(request);
  CHECK(outcome.text == "listo");
  REQUIRE(store->records.size() == 2);
  CHECK(store->records[0].role == ConversationTurnRole::User);
  CHECK(store->records[0].content == "mi clave es [omitida]");
  CHECK(store->records[0].content != "mi clave es 4421");
  REQUIRE(!modelSaw.empty());
  CHECK(modelSaw.back().content == "mi clave es [omitida]");
}

TEST_CASE("a refused conversation is refused before any stream or turn starts, and the store is not touched")
{
  LlmController controller;
  controller.setTurnEngine(answer);
  CHECK_THROWS_AS(controller.requireConversationAccess(chatFrom(true)), ResponseException);

  const auto store = std::make_shared<FakeStore>();
  controller.setConversation(store);
  CHECK_NOTHROW(controller.requireConversationAccess(chatFrom(true)));
  CHECK_NOTHROW(controller.requireConversationAccess(chatFrom(false)));

  ChatRequest noUser = chatFrom(true);
  noUser.userId = 0;
  CHECK_THROWS_AS(controller.requireConversationAccess(noUser), ResponseException);

  ChatRequest noLeg = chatFrom(true);
  noLeg.conversationLeg.reset();
  CHECK_THROWS_AS(controller.requireConversationAccess(noLeg), ResponseException);
  CHECK(store->untouched());
}

TEST_CASE("use_conversation without a store is refused, never quietly off")
{
  LlmController controller;
  controller.setTurnEngine(answer);
  CHECK_THROWS_AS(controller.chatSync(chatFrom(true)), ResponseException);
}

TEST_CASE("an identified turn is mirrored to the inbox with the screened words and the reply")
{
  LlmController controller;
  const auto transcript = std::make_shared<FakeTranscript>();
  controller.setTranscript(transcript);
  controller.setTurnEngine(answer);

  static_cast<void>(controller.chatSync(chatFrom(false)));
  REQUIRE(transcript->exchanges.size() == 1);
  CHECK(transcript->exchanges.front().userId == 7);
  CHECK(transcript->exchanges.front().userText == "hola");
  CHECK(transcript->exchanges.front().reply == "buenas");
  CHECK(transcript->exchanges.front().leg == ConversationLegKind::Phone);
}

TEST_CASE("a withheld turn or an anonymous one is never mirrored")
{
  LlmController controller;
  const auto transcript = std::make_shared<FakeTranscript>();
  controller.setTranscript(transcript);
  controller.setTurnEngine(answer);

  ChatRequest anonymous = chatFrom(false);
  anonymous.userId = 0;
  static_cast<void>(controller.chatSync(anonymous));
  CHECK(transcript->exchanges.empty());

  const WithholdingScreen screen;
  controller.useScreen(screen);
  static_cast<void>(controller.chatSync(chatFrom(false)));
  CHECK(transcript->exchanges.empty());
}
