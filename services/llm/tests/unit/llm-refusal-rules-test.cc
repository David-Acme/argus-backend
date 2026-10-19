#include <doctest/doctest.h>

#include <auth/module-snapshot.hxx>

#include <json/json.h>
#include <feature/llm/services/turn/refusal-rules.hxx>
#include <feature/llm/services/turn/speech-render.hxx>

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <test-support/value-of.hxx>

namespace
{

using turn::refusal::Ask;
using turn::refusal::Refusal;

ModuleSnapshot modulesWith(std::string_view id, bool enabled)
{
  ModuleFlags modules;
  modules.push_back(ModuleFlag{.id = std::string(id),
                               .enabled = enabled,
                               .lifecycle = {},
                               .dataPurgedAt = 0,
                               .roles = {},
                               .name = {},
                               .summary = {},
                               .intro = {},
                               .kind = {}});
  return ModuleSnapshot(std::move(modules));
}

ToolAudience audienceFor(UserRole role, bool surveillance)
{
  return {.role = role, .modules = modulesWith("surveillance", surveillance)};
}

std::optional<Refusal> ask(std::string_view utterance, UserRole role, std::string_view lang = "es")
{
  const ToolAudience audience = audienceFor(role, true);
  return turn::refusal::refusalFor({.utterance = utterance, .audience = audience, .lang = lang});
}

}

TEST_CASE("a relay request refuses and keeps the person named")
{
  const auto es = valueOf(ask("dile a María que llego tarde al almuerzo", UserRole::Owner));
  CHECK(es.reason == turn::refusal::kNoRelay);
  CHECK(es.name == "María");

  const auto twoWords = valueOf(ask("avísale a mi hijo que ya salí", UserRole::Owner));
  CHECK(twoWords.reason == turn::refusal::kNoRelay);
  CHECK(twoWords.name == "mi hijo");

  const auto english = valueOf(ask("Tell María I'll be late for lunch", UserRole::Owner, "en"));
  CHECK(english.reason == turn::refusal::kNoRelay);
  CHECK(english.name == "María");

  const auto followUp = valueOf(ask("¿entonces le avisas o no?", UserRole::Owner));
  CHECK(followUp.reason == turn::refusal::kNoRelay);
  CHECK(followUp.name.empty());
}

TEST_CASE("a reminder ask is not a relay")
{
  CHECK_FALSE(ask("avísame cuando salga la comida", UserRole::Owner).has_value());
  CHECK_FALSE(ask("remind me when the pasta is ready", UserRole::Owner, "en").has_value());
  CHECK_FALSE(ask("tell me when the meeting starts", UserRole::Owner, "en").has_value());
}

TEST_CASE("another person's data refuses for the roles that cannot read it")
{
  const auto where = valueOf(ask("¿dónde está David ahora?", UserRole::Resident));
  CHECK(where.reason == turn::refusal::kPrivatePerson);
  CHECK(where.name == "David");

  const auto english = valueOf(ask("Where is David right now?", UserRole::Resident, "en"));
  CHECK(english.reason == turn::refusal::kPrivatePerson);
  CHECK(english.name == "David");

  const auto reminders = valueOf(ask("¿qué recordatorios tiene David?", UserRole::Owner));
  CHECK(reminders.reason == turn::refusal::kPrivatePerson);
}

TEST_CASE("the owner keeps answering where a person is")
{
  CHECK_FALSE(ask("¿dónde está?", UserRole::Owner).has_value());
  CHECK_FALSE(ask("¿dónde está Tomás?", UserRole::Owner).has_value());
}

TEST_CASE("a security question refuses while surveillance is off")
{
  const ToolAudience off = audienceFor(UserRole::Guard, false);
  const auto news = valueOf(turn::refusal::refusalFor(
      {.utterance = "¿hay alguna novedad de seguridad?", .audience = off, .lang = "es"}));
  CHECK(news.reason == turn::refusal::kModuleOff);

  const ToolAudience on = audienceFor(UserRole::Guard, true);
  CHECK_FALSE(turn::refusal::refusalFor(
                  {.utterance = "¿está armada la vigilancia?", .audience = on, .lang = "es"})
                  .has_value());
}

TEST_CASE("asking for a reminder that has no way to be postponed refuses")
{
  const auto es = valueOf(ask("pospónlo", UserRole::Owner));
  CHECK(es.reason == turn::refusal::kNoAction);

  const auto en = valueOf(ask("Postpone it", UserRole::Owner, "en"));
  CHECK(en.reason == turn::refusal::kNoAction);

  CHECK_FALSE(ask("¿a qué hora era el recordatorio de las plantas?", UserRole::Owner).has_value());
  CHECK_FALSE(ask("¿qué tengo hoy en la agenda?", UserRole::Owner).has_value());

  const auto phrase = valueOf(ask("¿por qué no lo pasas para más tarde?", UserRole::Owner));
  CHECK(phrase.reason == turn::refusal::kNoAction);
  const auto later = valueOf(ask("can you move it for later", UserRole::Owner, "en"));
  CHECK(later.reason == turn::refusal::kNoAction);
  CHECK_FALSE(ask("pon un recordatorio para después de comer", UserRole::Owner).has_value());
  CHECK_FALSE(ask("remind me to call mom for later tonight", UserRole::Owner, "en").has_value());
}

TEST_CASE("a deterministic refusal is rendered without the model")
{
  const auto line = [](const turn::speech::Refused& refused, std::string_view lang) {
    return turn::speech::refusalLine({.refused = refused, .lang = lang});
  };
  const turn::speech::Refused relay{.tool = {},
                                    .reason = std::string(turn::refusal::kNoRelay),
                                    .name = "María"};
  const auto es = valueOf(line(relay, "es"));
  CHECK(es.find("Todavía no puedo pasarle un mensaje a María") != std::string::npos);
  CHECK(es.find("¿") != std::string::npos);
  const auto en = valueOf(line(relay, "en"));
  CHECK(en.find("María") != std::string::npos);

  const turn::speech::Refused priv{.tool = {},
                                   .reason = std::string(turn::refusal::kPrivatePerson),
                                   .name = "David"};
  const auto refusal = valueOf(line(priv, "es"));
  CHECK(refusal.find("No puedo decirte eso de David") != std::string::npos);
  CHECK(refusal.find('?') == std::string::npos);

  const turn::speech::Refused tool{.tool = "calendar.cancel_event", .reason = "not_found", .name = {}};
  CHECK_FALSE(line(tool, "es").has_value());
}

TEST_CASE("a verbless privacy phrase asks instead of acting")
{
  const auto es = valueOf(turn::refusal::polarityAskFor(
      {.utterance = "la privacidad de la cámara del garaje", .audience = audienceFor(UserRole::Owner, true), .lang = "es"}));
  CHECK(es.slot == "privacy");
  const auto question = valueOf(turn::speech::slotQuestion(
      {.ask = turn::speech::AskSlot{.slot = es.slot,
                                    .tool = es.tool,
                                    .knownArgs = Json::Value(Json::objectValue),
                                    .reason = turn::speech::AskReason::Missing,
                                    .dates = {},
                                    .options = {}},
       .lang = "es",
       .now = 0}));
  CHECK(question == "¿La activo o la quito?");
  CHECK(turn::speech::slotQuestion(
            {.ask = turn::speech::AskSlot{.slot = "when",
                                          .tool = "memory.remind",
                                          .knownArgs = Json::Value(Json::objectValue),
                                          .reason = turn::speech::AskReason::Missing,
                                          .dates = {},
                                          .options = {}},
             .lang = "es",
             .now = 0})
            .has_value() == false);
  const auto english = turn::refusal::polarityAskFor({.utterance = "the garage camera privacy",
                                                      .audience = audienceFor(UserRole::Owner, true),
                                                      .lang = "en"});
  CHECK(english.has_value());
  CHECK(turn::refusal::polarityAskFor({.utterance = "activa la privacidad de la cámara del garaje",
                                       .audience = audienceFor(UserRole::Owner, true),
                                       .lang = "es"})
            .has_value() == false);
  CHECK(turn::refusal::polarityAskFor(
            {.utterance = "quita la privacidad de la cámara del garaje",
             .audience = audienceFor(UserRole::Owner, true),
             .lang = "es"})
            .has_value() == false);
  CHECK_FALSE(turn::refusal::polarityAskFor(
                  {.utterance = "¿a qué hora era el recordatorio de las plantas?",
                   .audience = audienceFor(UserRole::Owner, true),
                   .lang = "es"})
                  .has_value());
}

TEST_CASE("a relay with no name in front keeps the person out of the line")
{
  const auto es = valueOf(ask("¿entonces puedes avisarle o no?", UserRole::Owner));
  CHECK(es.reason == turn::refusal::kNoRelay);
  CHECK(es.name.empty());
  const auto pe = valueOf(ask("¿entonces le avisas o no?", UserRole::Owner, "es"));
  CHECK(pe.name.empty());
  const auto en = valueOf(ask("So can you tell them or not?", UserRole::Owner, "en"));
  CHECK(en.reason == turn::refusal::kNoRelay);
  CHECK(en.name.empty());
  const auto tell = valueOf(ask("dile a María que llegué", UserRole::Owner));
  CHECK(tell.name == "María");
}

TEST_CASE("presence is the owner's alone")
{
  for (const UserRole role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    const auto es = valueOf(ask("¿quiénes están en casa ahora?", role));
    CHECK(es.reason == turn::refusal::kPrivatePerson);
    const auto en = valueOf(ask("Who is home right now?", role, "en"));
    CHECK(en.reason == turn::refusal::kPrivatePerson);
  }
  CHECK_FALSE(ask("¿quiénes están en casa?", UserRole::Owner).has_value());
  CHECK_FALSE(ask("Who is home?", UserRole::Owner, "en").has_value());
}
