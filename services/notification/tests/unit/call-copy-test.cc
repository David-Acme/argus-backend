#include <doctest/doctest.h>

#include <feature/call/services/call-copy.hxx>
#include <feature/call/vocabulary/call-trigger.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
constexpr int64_t kNow = 1800000000;

struct KindCase
{
  const char* name;
  CallTrigger trigger;
  const char* lang;
  const char* user;
  Json::Value data;
  const char* reason;
};

std::string firstSentence(const std::string& text)
{
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch != '.' && ch != '!' && ch != '?')
      continue;
    if (i + 1 == text.size() || text[i + 1] == ' ')
      return text.substr(0, i + 1);
  }
  return text;
}

struct EpisodeInput
{
  const char* urgency;
  const char* phase;
};

Json::Value guardEpisode(const EpisodeInput& input)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_episode";
  data["urgency"] = input.urgency;
  data["phase"] = input.phase;
  data["episodeId"] = 9;
  data["environmentId"] = 1;
  data["cameraId"] = 6;
  data["cameraName"] = "Patio";
  data["subject"] = "stranger";
  data["people"] = 1;
  data["action"] = "speaker";
  data["reasons"] = Json::Value(Json::arrayValue);
  data["reasons"].append("night");
  return data;
}

struct PanicInput
{
  const char* actor;
  const char* environment;
};

Json::Value guardPanic(const PanicInput& input)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_panic";
  data["actorName"] = input.actor;
  data["environmentName"] = input.environment;
  return data;
}

Json::Value guardDuress()
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_duress";
  data["actorName"] = "Tom";
  data["environmentName"] = "Casa";
  return data;
}

Json::Value guardTamper(const char* camera)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_tamper";
  data["cameraName"] = camera;
  return data;
}

Json::Value arrival(const char* person)
{
  Json::Value data(Json::objectValue);
  data["personName"] = person;
  data["environmentName"] = "Casa";
  return data;
}

Json::Value agendaEvent(const char* title, int64_t startsAt)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "agenda_event";
  data["title"] = title;
  data["startsAt"] = static_cast<Json::Int64>(startsAt);
  return data;
}

Json::Value agendaReminder(const char* title)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "agenda_reminder";
  data["title"] = title;
  return data;
}

Json::Value assistantTopic(const char* topic)
{
  Json::Value data(Json::objectValue);
  data["topic"] = topic;
  return data;
}

Json::Value discreetEpisode()
{
  Json::Value data = guardEpisode({.urgency = "critical", .phase = "opened"});
  data["discreet"] = true;
  return data;
}

CallCopy renderOf(const KindCase& kind)
{
  return call_copy::render({.trigger = kind.trigger,
                            .lang = kind.lang,
                            .data = kind.data,
                            .userName = kind.user,
                            .now = kNow});
}

std::vector<KindCase> kindCases()
{
  return {
      {.name = "a critical guard episode",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardEpisode({.urgency = "critical", .phase = "opened"}),
       .reason = "hay una persona desconocida"},
      {.name = "a guard episode while nobody is home",
       .trigger = CallTrigger::GuardIntruder,
       .lang = "es",
       .user = "Laura",
       .data = guardEpisode({.urgency = "time_sensitive", .phase = "opened"}),
       .reason = "hay una persona desconocida"},
      {.name = "a guard episode that got worse",
       .trigger = CallTrigger::GuardEscalation,
       .lang = "es",
       .user = "Laura",
       .data = guardEpisode({.urgency = "critical", .phase = "escalated"}),
       .reason = "hay una persona desconocida"},
      {.name = "a discreet guard episode",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = discreetEpisode(),
       .reason = "hay una persona desconocida"},
      {.name = "a panic button",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardPanic({.actor = "Tom", .environment = "Casa"}),
       .reason = "Tom pulsó el botón de pánico"},
      {.name = "a silent alert",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardDuress(),
       .reason = "Tom ha enviado una alerta silenciosa"},
      {.name = "a camera that stopped seeing",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardTamper("Patio"),
       .reason = "la cámara Patio ha dejado de ver"},
      {.name = "someone arriving",
       .trigger = CallTrigger::GuardArrival,
       .lang = "es",
       .user = "Laura",
       .data = arrival("Ana"),
       .reason = "ha llegado Ana"},
      {.name = "an agenda event in an hour",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaEvent("Dentista", kNow + 3600),
       .reason = "«Dentista»"},
      {.name = "an agenda reminder",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaReminder("Llamar al dentista"),
       .reason = "Llamar al dentista"},
      {.name = "an assistant call with a topic",
       .trigger = CallTrigger::Assistant,
       .lang = "es",
       .user = "Laura",
       .data = assistantTopic("llamar al dentista"),
       .reason = "llamar al dentista"},
      {.name = "an assistant call without a topic",
       .trigger = CallTrigger::Assistant,
       .lang = "es",
       .user = "Laura",
       .data = Json::Value(Json::objectValue),
       .reason = "¿en qué te ayudo?"},
      {.name = "a name that carries a period",
       .trigger = CallTrigger::GuardArrival,
       .lang = "es",
       .user = "Dra. García",
       .data = arrival("Ana"),
       .reason = "ha llegado Ana"},
      {.name = "an arrival in English",
       .trigger = CallTrigger::GuardArrival,
       .lang = "en",
       .user = "Laura",
       .data = arrival("Ana"),
       .reason = "Ana arrived"},
      {.name = "an assistant call with a topic in English",
       .trigger = CallTrigger::Assistant,
       .lang = "en",
       .user = "Laura",
       .data = assistantTopic("call the dentist"),
       .reason = "call the dentist"},
      {.name = "a person name that carries a period",
       .trigger = CallTrigger::GuardArrival,
       .lang = "es",
       .user = "Laura",
       .data = arrival("Dr. Pérez"),
       .reason = "ha llegado Dr Pérez"},
      {.name = "a doer name that carries a period",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardPanic({.actor = "Dr. Pérez", .environment = "Casa"}),
       .reason = "Dr Pérez pulsó el botón de pánico"},
      {.name = "an environment that carries a period",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardPanic({.actor = "Tom", .environment = "Casa."}),
       .reason = "pulsó el botón de pánico en Casa"},
      {.name = "a camera that carries a period",
       .trigger = CallTrigger::GuardCritical,
       .lang = "es",
       .user = "Laura",
       .data = guardTamper("Patio."),
       .reason = "la cámara Patio ha dejado de ver"},
      {.name = "a reminder title that carries a period",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaReminder("Llamar al Dr. Pérez"),
       .reason = "Llamar al Dr Pérez"},
      {.name = "a reminder title that ends in a question",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaReminder("Llamar al dentista?"),
       .reason = "Llamar al dentista"},
      {.name = "an event title that carries a period",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaEvent("Cita c. dentista", kNow),
       .reason = "«Cita c dentista»"},
      {.name = "a displayed title keeps the question the opening drops",
       .trigger = CallTrigger::Agenda,
       .lang = "es",
       .user = "Laura",
       .data = agendaReminder("¿Pagaste el arbitrio?"),
       .reason = "¿Pagaste el arbitrio"},
  };
}
}

TEST_CASE("every call kind's opening says the reason in its first sentence")
{
  static_assert(kCallTriggers.size() ==
                static_cast<std::size_t>(CallTrigger::Assistant) + 1);
  const std::vector<KindCase> cases = kindCases();
  for (const KindCase& kind : cases) {
    INFO(kind.name);
    const CallCopy copy = renderOf(kind);
    CHECK(firstSentence(copy.openingLine).find(kind.reason) != std::string::npos);
  }
  for (const CallTrigger trigger : kCallTriggers) {
    CAPTURE(callTriggerToString(trigger));
    bool covered = false;
    for (const KindCase& kind : cases)
      covered = covered || kind.trigger == trigger;
    CHECK(covered);
  }
}

TEST_CASE("the opening texts are pinned")
{
  const int64_t at = kNow + 3600;
  const std::vector<KindCase> cases = kindCases();
  const std::vector<std::string> pinned{
      "Hola, Laura, te llamo por algo urgente de la vigilancia: hay una persona desconocida en Patio, de noche. Le estoy avisando por el altavoz. ¿Quieres que te muestre la cámara?",
      "Hola, Laura, te llamo por la vigilancia: hay una persona desconocida en Patio, de noche. Le estoy avisando por el altavoz. ¿Quieres que te muestre la cámara?",
      "Hola, Laura, te llamo porque la situación ha empeorado: hay una persona desconocida en Patio, de noche. Le estoy avisando por el altavoz. ¿Quieres que te muestre la cámara?",
      "Hola, Laura, te aviso en voz baja: hay una persona desconocida en Patio. No abras y quédate dentro. ¿Quieres ver la cámara?",
      "Hola, Laura, Tom pulsó el botón de pánico en Casa.",
      "Hola, Laura, Tom ha enviado una alerta silenciosa en Casa; puede estar bajo amenaza, no le llames.",
      "Hola, Laura, la cámara Patio ha dejado de ver; puede estar tapada o desconectada.",
      "Hola, Laura, te aviso de que ha llegado Ana a Casa.",
      "Hola, Laura, te llamo por tu agenda: a las " + call_copy::clockTime(at) +
          " tienes «Dentista».",
      "Hola, Laura, te recuerdo: Llamar al dentista.",
      "Hola, Laura, me pediste que te llamara para recordarte: llamar al dentista.",
      "Hola, Laura, ¿en qué te ayudo?",
      "Hola, Dra García, te aviso de que ha llegado Ana a Casa.",
      "Hi Laura, I am letting you know that Ana arrived at Casa.",
      "Hi Laura, you asked me to call you to remind you: call the dentist.",
      "Hola, Laura, te aviso de que ha llegado Dr Pérez a Casa.",
      "Hola, Laura, Dr Pérez pulsó el botón de pánico en Casa.",
      "Hola, Laura, Tom pulsó el botón de pánico en Casa.",
      "Hola, Laura, la cámara Patio ha dejado de ver; puede estar tapada o desconectada.",
      "Hola, Laura, te recuerdo: Llamar al Dr Pérez.",
      "Hola, Laura, te recuerdo: Llamar al dentista.",
      "Hola, Laura, te llamo por tu agenda: ahora empieza «Cita c dentista».",
      "Hola, Laura, te recuerdo: ¿Pagaste el arbitrio.",
  };
  REQUIRE(cases.size() == pinned.size());
  for (std::size_t i = 0; i < cases.size(); ++i) {
    INFO(cases[i].name);
    CHECK(renderOf(cases[i]).openingLine == pinned[i]);
  }
}

TEST_CASE("the display keeps the punctuation the spoken opening drops")
{
  const Json::Value reminder = agendaReminder("¿Pagaste el arbitrio?");
  const CallCopy copy = call_copy::render({.trigger = CallTrigger::Agenda,
                                           .lang = "es",
                                           .data = reminder,
                                           .userName = "Laura",
                                           .now = kNow});
  CHECK(copy.title == "Recordatorio · ¿Pagaste el arbitrio?");
  CHECK(copy.summary == "¿Pagaste el arbitrio?");
  CHECK(copy.openingLine == "Hola, Laura, te recuerdo: ¿Pagaste el arbitrio.");
}
