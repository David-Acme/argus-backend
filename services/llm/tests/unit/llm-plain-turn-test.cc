#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>

#include <string>
#include <vector>

namespace
{

struct ScriptedEngine
{
  std::vector<ChatRequest> requests;
  std::vector<std::string> tokens;
  std::string reply;

  ChatEngine engine()
  {
    return {.chat = [this](const ChatRequest& request) { return answered(request); },
            .chatStream =
                [this](const ChatRequest& request, const TokenCallback& onToken) {
                  requests.push_back(request);
                  for (const std::string& token : tokens)
                    onToken(token, false);
                  onToken("", true);
                }};
  }

private:
  std::string answered(const ChatRequest& request)
  {
    requests.push_back(request);
    return reply;
  }
};

struct PlainTurn
{
  ToolRegistry registry;
  ScriptedEngine engine;
  LfmAdapter adapter;

  PlainTurn() : adapter({.engine = engine.engine(), .registry = registry, .router = nullptr}) {}

  ChatRequest request(const std::string& utterance, bool toolsEnabled = true)
  {
    ChatRequest ask;
    ask.messages = {{.role = "system", .content = "persona"}, {.role = "user", .content = utterance}};
    ask.lang = "es";
    ask.toolsEnabled = toolsEnabled;
    return ask;
  }
};
}

TEST_CASE("a tool-less turn carries the clock note the adapter puts at the head of the last user message")
{
  PlainTurn turn;
  turn.engine.reply = "Son las tres.";
  const ToolChatOutput asked = turn.adapter.chatPlain(turn.request("¿Qué hora es?"));
  REQUIRE(turn.engine.requests.size() == 1);
  const std::vector<ChatMessage>& sent = turn.engine.requests.front().messages;
  REQUIRE(sent.size() == 2);
  CHECK(sent[0].role == "system");
  CHECK(sent[1].role == "user");
  CHECK(sent[1].content.starts_with("Referencia, menciónala solo si te preguntan la fecha o la hora: hoy es "));
  CHECK(sent[1].content.find("¿Qué hora es?") != std::string::npos);
  CHECK(asked.reply == "Son las tres.");

  turn.engine.requests.clear();
  turn.adapter.chatPlain(turn.request("Cuéntame un chiste."));
  REQUIRE(turn.engine.requests.size() == 1);
  CHECK(turn.engine.requests.front().messages.size() == 2);
}

TEST_CASE("a tool-less turn keeps the claim gate and the offer strip the tool path has")
{
  PlainTurn turn;
  turn.engine.reply = "Listo, lo anoté.";
  const ToolChatOutput claim = turn.adapter.chatPlain(turn.request("¿Qué tengo pendiente?"));
  CHECK(claim.reply == "No pude hacerlo. ¿Lo intento de nuevo?");
  CHECK(claim.rawReply == "Listo, lo anoté.");

  turn.engine.reply = "Hoy es miércoles. ¿Necesitas algo más?";
  const ToolChatOutput stripped = turn.adapter.chatPlain(turn.request("Cuéntame un chiste."));
  CHECK(stripped.reply == "Hoy es miércoles.");
  CHECK(stripped.rawReply == "Hoy es miércoles. ¿Necesitas algo más?");

  turn.engine.reply = "Hecho. Recuerda que el lunes necesitas algo de efectivo.";
  const ToolChatOutput kept = turn.adapter.chatPlain(turn.request("Cuéntame un chiste."));
  CHECK(kept.reply == "Hecho. Recuerda que el lunes necesitas algo de efectivo.");
}

TEST_CASE("a turn with tools turned off keeps its claim and still loses the trailing offer")
{
  PlainTurn turn;
  turn.engine.reply = "Listo, lo anoté.";
  const ToolChatOutput claim = turn.adapter.chatPlain(turn.request("¿Qué tengo pendiente?", false));
  CHECK(claim.reply == "Listo, lo anoté.");

  turn.engine.reply = "Hoy es miércoles. ¿Necesitas algo más?";
  const ToolChatOutput stripped = turn.adapter.chatPlain(turn.request("Cuéntame un chiste.", false));
  CHECK(stripped.reply == "Hoy es miércoles.");
}

TEST_CASE("a streamed tool-less turn reaches the caller through the same gate")
{
  PlainTurn turn;
  turn.engine.tokens = {"Hoy es ", "miércoles. ", "¿Necesitas ", "algo más?"};
  std::string heard;
  std::size_t done = 0;
  const ToolChatOutput streamed = turn.adapter.chatPlainStream(
      {.request = turn.request("Cuéntame un chiste."),
       .onToken = [&heard, &done](const std::string& token, bool finished) {
         heard += token;
         done += finished ? 1 : 0;
       }});
  CHECK(streamed.reply == "Hoy es miércoles.");
  CHECK(streamed.rawReply == "Hoy es miércoles. ¿Necesitas algo más?");
  CHECK(heard == "Hoy es miércoles.");
  CHECK(done == 1);
}
