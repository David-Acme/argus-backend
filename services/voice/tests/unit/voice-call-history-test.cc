#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/voice/call-history.hxx>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

std::vector<std::string> roles(const std::vector<ChatMessage>& messages)
{
  std::vector<std::string> out;
  out.reserve(messages.size());
  for (const auto& message : messages)
    out.push_back(message.role);
  return out;
}

bool startsWith(const std::vector<ChatMessage>& longer, const std::vector<ChatMessage>& prefix)
{
  if (prefix.size() > longer.size())
    return false;
  return std::equal(prefix.begin(), prefix.end(), longer.begin(), [](const ChatMessage& a, const ChatMessage& b) {
    return a.role == b.role && a.content == b.content;
  });
}

std::string readFile(const char* path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Exchange
{
  std::string said;
  std::string answer;
};

void turn(CallHistory& history, const Exchange& exchange)
{
  history.addUser(exchange.said);
  history.addTone("\n(Tono: cálido y corto.)");
  static_cast<void>(history.request());
  history.addAssistant(exchange.answer);
  history.trim();
}

}

TEST_CASE("The user message carries only what was heard and the tone note follows it")
{
  CallHistory history(VoiceLang::Es);
  history.addAssistant("Hola, soy Argus.");
  history.addUser("recuerda que mi hermana viene los domingos");
  history.addTone("\n(Tono: cálido y corto.)");
  const auto messages = history.request();
  REQUIRE(messages.size() == 4);
  CHECK(roles(messages) == std::vector<std::string>{"system", "assistant", "user", "system"});
  CHECK(messages[2].content == "recuerda que mi hermana viene los domingos");
  CHECK(messages[3].content == "(Tono: cálido y corto.)");
}

TEST_CASE("Notes before the first request fold into the prompt; later ones append")
{
  CallHistory history(VoiceLang::Es);
  history.addAssistant("Hola.");
  history.addNote("Cámaras de la casa: Entrada, Patio.");
  history.setSituation("Modo de vigilancia: en casa.");
  CHECK(history.size() == 2);
  CHECK(history.entries().front().message.content.find("Cámaras de la casa: Entrada, Patio.") != std::string::npos);
  CHECK(history.entries().front().message.content.find("Modo de vigilancia: en casa.") != std::string::npos);
  CHECK(history.entries().front().message.content.find("Lo que sabes ahora mismo por la app") != std::string::npos);
  const std::string firstRequest = readFile(ARGUS_CALL_PROMPT_ES) + "\n" + readFile(ARGUS_CALL_KNOWN_ES) + "\n" +
                                   "Cámaras de la casa: Entrada, Patio." + "\n" + "Modo de vigilancia: en casa.";
  CHECK(history.entries().front().message.content == firstRequest);

  turn(history, {.said = "hola", .answer = "Hola, ¿qué tal?"});
  const auto first = history.request();

  history.addNote("Cámaras de la casa: Entrada, Patio.");
  history.setSituation("Modo de vigilancia: en casa.");
  CHECK(history.request().size() == first.size());

  history.setSituation("Modo de vigilancia: fuera.");
  history.addNote("El usuario está en el coche.");
  const auto second = history.request();
  CHECK(startsWith(second, first));
  REQUIRE(second.size() == first.size() + 2);
  CHECK(second[first.size()].role == "system");
  CHECK(second[first.size()].content == "Nota de la app (menciónala solo si viene al caso): Modo de vigilancia: fuera.");
  CHECK(second[first.size() + 1].content ==
        "Nota de la app (menciónala solo si viene al caso): El usuario está en el coche.");
}

TEST_CASE("Every turn extends the previous prompt until the history trims")
{
  CallHistory history(VoiceLang::En);
  history.addAssistant("Hi.");
  std::vector<ChatMessage> previous = history.request();
  for (int i = 0; i < 10; ++i) {
    turn(history, {.said = "question " + std::to_string(i), .answer = "answer " + std::to_string(i)});
    const auto current = history.request();
    CHECK(startsWith(current, previous));
    previous = current;
  }
  CHECK(history.userTurns() == 10);
}

TEST_CASE("A trim keeps five whole turns and a digest of what it dropped")
{
  CallHistory history(VoiceLang::Es);
  history.addAssistant("Hola.");
  turn(history, {.said = "hola", .answer = "Hola."});
  history.addNote("Cámaras de la casa: Entrada.");
  history.setSituation("Modo de vigilancia: noche.");
  history.addEvent("The app could not complete app.show_camera.");
  for (int i = 0; i < 10; ++i)
    turn(history, {.said = "pregunta " + std::to_string(i), .answer = "respuesta " + std::to_string(i)});

  CHECK(history.userTurns() == 5);
  const auto& entries = history.entries();
  CHECK(entries[0].kind == CallEntryKind::Prompt);
  CHECK(entries[1].kind == CallEntryKind::User);
  CHECK(entries[1].message.content == "pregunta 5");
  CHECK(entries.back().kind == CallEntryKind::Assistant);
  CHECK(std::ranges::none_of(entries, [](const CallEntry& entry) {
    return entry.kind == CallEntryKind::Note || entry.kind == CallEntryKind::Situation;
  }));

  const std::string& prompt = entries[0].message.content;
  CHECK(prompt.find("Cámaras de la casa: Entrada.") != std::string::npos);
  CHECK(prompt.find("Modo de vigilancia: noche.") != std::string::npos);
  CHECK(prompt.find("Antes en esta llamada") != std::string::npos);
  CHECK(prompt.find("- Usuario: pregunta 4") != std::string::npos);
  CHECK(prompt.find("- Tú: respuesta 4") != std::string::npos);
  CHECK(prompt.find("- App: The app could not complete app.show_camera.") != std::string::npos);
  CHECK(prompt.find("(Tono") == std::string::npos);
}

TEST_CASE("A rolled-back turn leaves neither the user words nor their tone note")
{
  CallHistory history(VoiceLang::Es);
  history.addAssistant("Hola.");
  history.addUser("¿qué hora es?");
  history.addTone("(Tono: esto es urgente.)");
  history.rollbackUser();
  CHECK(history.size() == 2);
  CHECK(history.entries().back().kind == CallEntryKind::Assistant);

  history.rollbackUser();
  CHECK(history.size() == 2);
}

TEST_CASE("Sanitizing keeps whole UTF-8 characters")
{
  const std::string text = std::string(9, 'x') + "á";
  CHECK(utf8Prefix(text, 10) == std::string(9, 'x'));
  CHECK(utf8Prefix(text, 11) == text);
  CHECK(sanitizedLine(" uno\ndos\t ", 50) == "uno dos");
  CHECK(sanitizedBlock(" uno\ndos\t \n", 50) == "uno\ndos");
}

TEST_CASE("The prompt states the call's language and is pinned to its fixtures")
{
  const std::string spanish = callSystemPrompt(VoiceLang::Es);
  CHECK(spanish.starts_with("You are Argus"));
  CHECK(spanish.find("Reply strictly in Spanish") != std::string::npos);
  CHECK(spanish.find("Reply strictly in English") == std::string::npos);

  const std::string english = callSystemPrompt(VoiceLang::En);
  CHECK(english.find("Reply strictly in English") != std::string::npos);
  CHECK(english.find("Reply strictly in Spanish") == std::string::npos);

  CHECK(callSystemPrompt(VoiceLang::Es) == readFile(ARGUS_CALL_PROMPT_ES));
  CHECK(callSystemPrompt(VoiceLang::En) == readFile(ARGUS_CALL_PROMPT_EN));
}
