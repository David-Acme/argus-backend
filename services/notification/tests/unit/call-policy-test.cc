#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/call/schemas/call/call-schema.hxx>
#include <feature/call/services/call-copy.hxx>
#include <feature/call/services/call-policy.hxx>
#include <feature/call/services/response-copy.hxx>
#include <feature/call/schemas/call-response/call-response-schema.hxx>
#include <feature/call/services/call-trigger-classifier.hxx>

#include <string>
#include <utility>

namespace
{
constexpr int64_t kNow = 1800000000;

struct Scenario
{
  CallTrigger trigger{CallTrigger::GuardIntruder};
  bool critical{false};
  int hour{12};
  int weekday{3};
  int64_t environmentId{0};
  bool alreadyCalled{false};
  bool ringing{false};
  int64_t lastCallAt{0};
  int callsLastHour{0};
  bool enabled{true};
};

CallVerdict run(const CallPreferenceSchema& preference, const Scenario& scenario)
{
  return call_policy::decide({.trigger = scenario.trigger,
                              .critical = scenario.critical,
                              .preference = preference,
                              .localHour = scenario.hour,
                              .localWeekday = scenario.weekday,
                              .now = kNow,
                              .environmentId = scenario.environmentId,
                              .alreadyCalled = scenario.alreadyCalled,
                              .ringing = scenario.ringing,
                              .lastCallAt = scenario.lastCallAt,
                              .callsLastHour = scenario.callsLastHour,
                              .limits = {.enabled = scenario.enabled,
                                         .callGapS = 300,
                                         .maxCallsPerHour = 4}});
}

struct GuardDataInput
{
  std::string urgency;
  std::string phase;
};

Json::Value guardData(const GuardDataInput& input)
{
  const std::string& urgency = input.urgency;
  const std::string& phase = input.phase;
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_episode";
  data["urgency"] = urgency;
  data["phase"] = phase;
  data["threadKey"] = "guard:episode:41";
  data["episodeId"] = 41;
  data["environmentId"] = 2;
  data["cameraId"] = 6;
  data["cameraName"] = "Patio";
  data["environmentName"] = "Casa";
  data["subject"] = "stranger";
  data["people"] = 1;
  data["action"] = "speaker";
  Json::Value reasons(Json::arrayValue);
  reasons.append("night");
  data["reasons"] = std::move(reasons);
  data["lang"] = "es";
  return data;
}
}

TEST_CASE("quiet hours: a window may wrap midnight, -1 or an empty window is off")
{
  CHECK(call_policy::inQuietHours({.hour = 23, .weekday = 3, .startHour = 22, .endHour = 7, .days = 0x7F}));
  CHECK(call_policy::inQuietHours({.hour = 3, .weekday = 3, .startHour = 22, .endHour = 7, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 7, .weekday = 3, .startHour = 22, .endHour = 7, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 12, .weekday = 3, .startHour = 22, .endHour = 7, .days = 0x7F}));
  CHECK(call_policy::inQuietHours({.hour = 8, .weekday = 3, .startHour = 8, .endHour = 17, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 17, .weekday = 3, .startHour = 8, .endHour = 17, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 3, .weekday = 3, .startHour = -1, .endHour = 7, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 3, .weekday = 3, .startHour = 22, .endHour = -1, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 3, .weekday = 3, .startHour = 5, .endHour = 5, .days = 0x7F}));
  CHECK_FALSE(call_policy::inQuietHours({.hour = 3, .weekday = 3, .startHour = 24, .endHour = 7, .days = 0x7F}));
}

TEST_CASE("quiet days: a window belongs to the day it starts")
{
  constexpr int kWeekdays = 0b0111110;
  CHECK(call_policy::inQuietHours(
      {.hour = 23, .weekday = 1, .startHour = 22, .endHour = 7, .days = kWeekdays}));
  CHECK_FALSE(call_policy::inQuietHours(
      {.hour = 23, .weekday = 6, .startHour = 22, .endHour = 7, .days = kWeekdays}));
  CHECK(call_policy::inQuietHours(
      {.hour = 3, .weekday = 6, .startHour = 22, .endHour = 7, .days = kWeekdays}));
  CHECK_FALSE(call_policy::inQuietHours(
      {.hour = 3, .weekday = 0, .startHour = 22, .endHour = 7, .days = kWeekdays}));
  CHECK_FALSE(call_policy::inQuietHours(
      {.hour = 10, .weekday = 0, .startHour = 9, .endHour = 18, .days = kWeekdays}));
  CHECK(call_policy::inQuietHours(
      {.hour = 10, .weekday = 2, .startHour = 9, .endHour = 18, .days = kWeekdays}));
  CHECK_FALSE(call_policy::inQuietHours(
      {.hour = 23, .weekday = 1, .startHour = 22, .endHour = 7, .days = 0}));

  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.quietStartHour = 22;
  preference.quietEndHour = 7;
  preference.quietDays = kWeekdays;
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .hour = 23, .weekday = 2})
            .reason == "quiet_hours");
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .hour = 23, .weekday = 6})
            .decision == CallDecision::Ring);
}

TEST_CASE("a user who keeps live calls quiet is never injected into")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda}).injectable);
  preference.liveAnnounce = false;
  const auto verdict = run(preference, {.trigger = CallTrigger::Agenda});
  CHECK(verdict.decision == CallDecision::Ring);
  CHECK_FALSE(verdict.injectable);
  preference.dndUntil = kNow + 60;
  CHECK_FALSE(run(preference, {.trigger = CallTrigger::Agenda}).injectable);
}

TEST_CASE("per-user ring and push timing stay inside their bounds")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  CHECK(preference.ringSecondsClamped() == 45);
  CHECK(preference.pushDelayClamped() == 4);
  CHECK(preference.agendaLeadMinutes == 10);
  CHECK(preference.quietDays == 0x7F);
  CHECK(preference.lang.empty());
  preference.ringSeconds = 5;
  preference.pushDelaySeconds = 99;
  CHECK(preference.ringSecondsClamped() == 20);
  CHECK(preference.pushDelayClamped() == 30);
  preference.ringSeconds = 500;
  preference.pushDelaySeconds = -3;
  CHECK(preference.ringSecondsClamped() == 90);
  CHECK(preference.pushDelayClamped() == 0);
}

TEST_CASE("defaults ring for guard and agenda, never for arrivals")
{
  const auto preference = CallPreferenceSchema::defaultsFor(7);
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical, .critical = true})
            .decision == CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::GuardIntruder}).decision ==
        CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::GuardEscalation}).decision ==
        CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda}).decision ==
        CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::Assistant}).decision ==
        CallDecision::Ring);
  const auto arrival = run(preference, {.trigger = CallTrigger::GuardArrival});
  CHECK(arrival.decision == CallDecision::Drop);
  CHECK(arrival.reason == "trigger_off");
}

TEST_CASE("one call per episode: an episode that already called is dropped")
{
  const auto preference = CallPreferenceSchema::defaultsFor(7);
  const auto verdict = run(preference, {.trigger = CallTrigger::GuardEscalation,
                                        .critical = true,
                                        .alreadyCalled = true});
  CHECK(verdict.decision == CallDecision::Drop);
  CHECK(verdict.reason == "already_called");
  CHECK_FALSE(verdict.injectable);
}

TEST_CASE("per-trigger preferences: notify keeps the notification, off drops it")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.agenda = CallMode::Notify;
  preference.guardIntruder = CallMode::Off;
  const auto agenda = run(preference, {.trigger = CallTrigger::Agenda});
  CHECK(agenda.decision == CallDecision::Notify);
  CHECK(agenda.reason == "trigger_notify");
  CHECK_FALSE(agenda.injectable);
  CHECK(run(preference, {.trigger = CallTrigger::GuardIntruder}).decision ==
        CallDecision::Drop);
  preference.guardArrival = CallMode::Call;
  CHECK(run(preference, {.trigger = CallTrigger::GuardArrival}).decision ==
        CallDecision::Ring);
}

TEST_CASE("calls switched off globally or by the user fall back to notifications")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical,
                         .critical = true,
                         .enabled = false})
            .reason == "calls_disabled");
  preference.enabled = false;
  const auto verdict =
      run(preference, {.trigger = CallTrigger::GuardCritical, .critical = true});
  CHECK(verdict.decision == CallDecision::Notify);
  CHECK(verdict.reason == "calls_off");
}

TEST_CASE("a muted environment never rings for guard, and only for guard")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.mutedEnvironmentIds = {3};
  const auto muted = run(preference, {.trigger = CallTrigger::GuardCritical,
                                      .critical = true,
                                      .environmentId = 3});
  CHECK(muted.decision == CallDecision::Notify);
  CHECK(muted.reason == "environment_muted");
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical,
                         .critical = true,
                         .environmentId = 2})
            .decision == CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .environmentId = 3})
            .decision == CallDecision::Ring);
}

TEST_CASE("do not disturb holds everything but a critical alert, unless asked")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.dndUntil = kNow + 600;
  const auto intruder = run(preference, {.trigger = CallTrigger::GuardIntruder});
  CHECK(intruder.decision == CallDecision::Notify);
  CHECK(intruder.reason == "do_not_disturb");
  CHECK(intruder.injectable);
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical, .critical = true})
            .decision == CallDecision::Ring);
  preference.criticalBypass = false;
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical, .critical = true})
            .reason == "do_not_disturb");
  preference.dndUntil = kNow - 1;
  CHECK(run(preference, {.trigger = CallTrigger::GuardIntruder}).decision ==
        CallDecision::Ring);
}

TEST_CASE("quiet hours turn non-critical calls into notifications")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.quietStartHour = 22;
  preference.quietEndHour = 7;
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .hour = 23}).reason ==
        "quiet_hours");
  CHECK(run(preference, {.trigger = CallTrigger::GuardIntruder, .hour = 2})
            .reason == "quiet_hours");
  CHECK(run(preference,
            {.trigger = CallTrigger::GuardCritical, .critical = true, .hour = 2})
            .decision == CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .hour = 9}).decision ==
        CallDecision::Ring);
}

TEST_CASE("a call already ringing turns the next item into its follow-up")
{
  const auto preference = CallPreferenceSchema::defaultsFor(7);
  const auto verdict = run(preference, {.trigger = CallTrigger::Agenda,
                                        .ringing = true,
                                        .lastCallAt = kNow - 5,
                                        .callsLastHour = 9});
  CHECK(verdict.decision == CallDecision::Followup);
  CHECK(verdict.injectable);
}

TEST_CASE("cooldown and the hourly cap spare only critical alerts")
{
  const auto preference = CallPreferenceSchema::defaultsFor(7);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .lastCallAt = kNow - 120})
            .reason == "cooldown");
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .lastCallAt = kNow - 301})
            .decision == CallDecision::Ring);
  CHECK(run(preference, {.trigger = CallTrigger::Agenda, .callsLastHour = 4})
            .reason == "hourly_cap");
  CHECK(run(preference, {.trigger = CallTrigger::GuardCritical,
                         .critical = true,
                         .lastCallAt = kNow - 10,
                         .callsLastHour = 9})
            .decision == CallDecision::Ring);
}

TEST_CASE("guard notifications become call candidates by urgency and phase")
{
  const CallCandidate critical =
      call_trigger::fromNotification(guardData({.urgency = "critical", .phase = "opened"}))
          .value_or(CallCandidate{});
  REQUIRE_FALSE(critical.dedupeKey.empty());
  CHECK(critical.trigger == CallTrigger::GuardCritical);
  CHECK(critical.critical);
  CHECK(critical.dedupeKey == "guard:episode:41");
  CHECK(critical.environmentId == 2);

  const CallCandidate intruder =
      call_trigger::fromNotification(guardData({.urgency = "time_sensitive", .phase = "opened"}))
          .value_or(CallCandidate{});
  REQUIRE_FALSE(intruder.dedupeKey.empty());
  CHECK(intruder.trigger == CallTrigger::GuardIntruder);
  CHECK_FALSE(intruder.critical);

  const CallCandidate escalated =
      call_trigger::fromNotification(guardData({.urgency = "critical", .phase = "escalated"}))
          .value_or(CallCandidate{});
  REQUIRE_FALSE(escalated.dedupeKey.empty());
  CHECK(escalated.trigger == CallTrigger::GuardEscalation);
  CHECK(escalated.critical);

  CHECK_FALSE(call_trigger::fromNotification(guardData({.urgency = "active", .phase = "opened"})));
  auto tamper = guardData({.urgency = "active", .phase = "opened"});
  tamper["kind"] = "guard_tamper";
  CHECK_FALSE(call_trigger::fromNotification(tamper));
  tamper["urgency"] = "time_sensitive";
  CHECK_FALSE(call_trigger::fromNotification(tamper));
  tamper["urgency"] = "critical";
  const CallCandidate tamperCall =
      call_trigger::fromNotification(tamper).value_or(CallCandidate{});
  CHECK(tamperCall.trigger == CallTrigger::GuardCritical);
  for (const char* kind : {"guard_panic", "guard_duress"}) {
    auto safety = guardData({.urgency = "critical", .phase = "opened"});
    safety["kind"] = kind;
    CHECK(call_trigger::fromNotification(safety).value_or(CallCandidate{}).trigger ==
          CallTrigger::GuardCritical);
    safety["urgency"] = "time_sensitive";
    CHECK_FALSE(call_trigger::fromNotification(safety));
  }
  auto unthreaded = guardData({.urgency = "critical", .phase = "opened"});
  unthreaded.removeMember("threadKey");
  unthreaded.removeMember("episodeId");
  CHECK_FALSE(call_trigger::fromNotification(unthreaded));
  CHECK_FALSE(call_trigger::fromNotification(Json::Value("x")));
}

TEST_CASE("agenda notifications become agenda candidates keyed by their thread")
{
  Json::Value data(Json::objectValue);
  data["kind"] = "agenda_event";
  data["threadKey"] = "agenda:event:9:1800000600";
  const CallCandidate candidate =
      call_trigger::fromNotification(data).value_or(CallCandidate{});
  REQUIRE_FALSE(candidate.dedupeKey.empty());
  CHECK(candidate.trigger == CallTrigger::Agenda);
  CHECK(candidate.dedupeKey == "agenda:event:9:1800000600");
  data.removeMember("threadKey");
  CHECK_FALSE(call_trigger::fromNotification(data));
}

TEST_CASE("the opening line carries the context in the user's language")
{
  const Json::Value data = guardData({.urgency = "critical", .phase = "opened"});
  const auto es = call_copy::render({.trigger = CallTrigger::GuardCritical,
                                     .lang = "es",
                                     .data = data,
                                     .userName = "Laura",
                                     .now = kNow});
  CHECK(es.openingLine.starts_with("Hola, Laura. "));
  CHECK(es.openingLine.find("Hay una persona desconocida en Patio (Casa), de noche.") !=
        std::string::npos);
  CHECK(es.openingLine.find("Le estoy avisando por el altavoz.") !=
        std::string::npos);
  CHECK(es.openingLine.ends_with("¿Quieres que te muestre la cámara?"));
  CHECK(es.title == "Una persona desconocida · Patio (Casa)");
  CHECK(es.missedLine ==
        "Te llamé porque había una persona desconocida en Patio (Casa).");
  CHECK(es.followupLine == "Además, hay una persona desconocida en Patio (Casa).");

  const auto en = call_copy::render({.trigger = CallTrigger::GuardIntruder,
                                     .lang = "en",
                                     .data = data,
                                     .userName = "",
                                     .now = kNow});
  CHECK(en.openingLine.starts_with("Hi. I am calling about the security."));
  CHECK(en.openingLine.find("There is an unknown person at Patio (Casa), at night.") !=
        std::string::npos);

  auto weapon = data;
  weapon["reasons"].append("weapon");
  weapon["action"] = "silent_weapon";
  const auto armed = call_copy::render({.trigger = CallTrigger::GuardCritical,
                                        .lang = "es",
                                        .data = weapon,
                                        .userName = "",
                                        .now = kNow});
  CHECK(armed.openingLine.find("y parece llevar un arma") != std::string::npos);
  CHECK(armed.openingLine.find("No le he dicho nada") != std::string::npos);
}

TEST_CASE("agenda, arrival and assistant copy read as a person would say them")
{
  Json::Value reminder(Json::objectValue);
  reminder["kind"] = "agenda_reminder";
  reminder["title"] = "Sacar la basura";
  const auto chore = call_copy::render({.trigger = CallTrigger::Agenda,
                                        .lang = "es",
                                        .data = reminder,
                                        .userName = "",
                                        .now = kNow});
  CHECK(chore.openingLine == "Hola. Te recuerdo: Sacar la basura.");

  Json::Value event(Json::objectValue);
  event["kind"] = "agenda_event";
  event["title"] = "Dentista";
  event["startsAt"] = static_cast<Json::Int64>(kNow + 600);
  event["location"] = "Calle Mayor";
  const auto later = call_copy::render({.trigger = CallTrigger::Agenda,
                                        .lang = "es",
                                        .data = event,
                                        .userName = "",
                                        .now = kNow});
  CHECK(later.openingLine.find("A las " + call_copy::clockTime(kNow + 600) +
                               " tienes «Dentista», en Calle Mayor.") !=
        std::string::npos);
  const auto now = call_copy::render({.trigger = CallTrigger::Agenda,
                                      .lang = "en",
                                      .data = event,
                                      .userName = "",
                                      .now = kNow + 590});
  CHECK(now.openingLine.find("\"Dentista\" is starting now, at Calle Mayor.") !=
        std::string::npos);

  Json::Value arrival(Json::objectValue);
  arrival["personName"] = "Marta";
  arrival["cameraName"] = "Entrada";
  const auto arrived = call_copy::render({.trigger = CallTrigger::GuardArrival,
                                          .lang = "es",
                                          .data = arrival,
                                          .userName = "Laura",
                                          .now = kNow});
  CHECK(arrived.openingLine == "Hola, Laura. Te aviso de que ha llegado Marta a Entrada.");
  CHECK(arrived.followupLine == "Además, ha llegado Marta a Entrada.");

  Json::Value topic(Json::objectValue);
  topic["topic"] = "llamar al dentista";
  const auto asked = call_copy::render({.trigger = CallTrigger::Assistant,
                                        .lang = "es",
                                        .data = topic,
                                        .userName = "",
                                        .now = kNow});
  CHECK(asked.openingLine ==
        "Hola. Me pediste que te llamara para recordarte: llamar al dentista.");
  CHECK(call_copy::joinFollowups("Hola.", {"Además, A.", "", "Además, B."}) ==
        "Hola. Además, A. Además, B.");
}

TEST_CASE("call ids round-trip and refuse anything else")
{
  CHECK(call_id::format(12) == "call-12");
  CHECK(call_id::parse("call-12") == 12);
  CHECK(call_id::parse("call-0") == 0);
  CHECK(call_id::parse("call--3") == 0);
  CHECK(call_id::parse("call-12x") == 0);
  CHECK(call_id::parse("rtc-12") == 0);
  CHECK(call_id::parse("call-") == 0);
  CHECK(call_id::parse("call-12345678901234567890") == 0);
}

TEST_CASE("a guard on duty is called whatever their own switches say, inside the system limits")
{
  auto preference = CallPreferenceSchema::defaultsFor(7);
  preference.guardIntruder = CallMode::Off;
  preference.enabled = false;
  preference.dndUntil = kNow + 600;
  preference.quietStartHour = 0;
  preference.quietEndHour = 23;
  preference.mutedEnvironmentIds = {4};
  const auto mandatory = [&](const Scenario& scenario) {
    return call_policy::decide({.trigger = scenario.trigger,
                                .critical = scenario.critical,
                                .preference = preference,
                                .localHour = scenario.hour,
                                .localWeekday = scenario.weekday,
                                .now = kNow,
                                .environmentId = scenario.environmentId,
                                .alreadyCalled = scenario.alreadyCalled,
                                .ringing = scenario.ringing,
                                .lastCallAt = scenario.lastCallAt,
                                .callsLastHour = scenario.callsLastHour,
                                .limits = {.enabled = scenario.enabled,
                                           .callGapS = 300,
                                           .maxCallsPerHour = 4},
                                .planNotify = false,
                                .mandatory = true});
  };
  const auto rings = mandatory({.trigger = CallTrigger::GuardIntruder, .environmentId = 4});
  CHECK(rings.decision == CallDecision::Ring);
  CHECK(rings.reason == "on_duty");
  CHECK(run(preference, {.trigger = CallTrigger::GuardIntruder}).decision == CallDecision::Drop);
  CHECK(mandatory({.trigger = CallTrigger::GuardIntruder, .alreadyCalled = true}).decision ==
        CallDecision::Drop);
  CHECK(mandatory({.trigger = CallTrigger::GuardIntruder, .enabled = false}).reason ==
        "calls_disabled");
  CHECK(mandatory({.trigger = CallTrigger::GuardIntruder, .lastCallAt = kNow - 10}).reason ==
        "cooldown");
  CHECK(mandatory({.trigger = CallTrigger::GuardCritical, .critical = true,
                   .lastCallAt = kNow - 10})
            .decision == CallDecision::Ring);
  CHECK(mandatory({.trigger = CallTrigger::GuardIntruder, .ringing = true}).decision ==
        CallDecision::Followup);
}

TEST_CASE("a plan that lists someone as notify keeps them to the notification")
{
  const auto preference = CallPreferenceSchema::defaultsFor(7);
  const auto verdict = call_policy::decide({.trigger = CallTrigger::GuardCritical,
                                            .critical = true,
                                            .preference = preference,
                                            .localHour = 12,
                                            .localWeekday = 3,
                                            .now = kNow,
                                            .environmentId = 0,
                                            .alreadyCalled = false,
                                            .ringing = false,
                                            .lastCallAt = 0,
                                            .callsLastHour = 0,
                                            .limits = {.enabled = true,
                                                       .callGapS = 300,
                                                       .maxCallsPerHour = 4},
                                            .planNotify = true,
                                            .mandatory = false});
  CHECK(verdict.decision == CallDecision::Notify);
  CHECK(verdict.reason == "plan_notify");
}

TEST_CASE("panic, duress and tamper read as what they are, and contacts are offered by name")
{
  Json::Value panic(Json::objectValue);
  panic["kind"] = "guard_panic";
  panic["actorName"] = "Tom";
  panic["environmentName"] = "Casa";
  const CallCopy es = call_copy::render(
      {.trigger = CallTrigger::GuardCritical, .lang = "es", .data = panic, .userName = "Laura", .now = kNow});
  CHECK(es.title == "Botón de pánico · Casa");
  CHECK(es.openingLine.find("Tom ha pulsado el botón de pánico en Casa") != std::string::npos);
  Json::Value duress = panic;
  duress["kind"] = "guard_duress";
  const CallCopy silent = call_copy::render(
      {.trigger = CallTrigger::GuardCritical, .lang = "en", .data = duress, .userName = "", .now = kNow});
  CHECK(silent.title == "Silent alert · Casa");
  CHECK(silent.summary.find("do not call them") != std::string::npos);
  Json::Value tamper(Json::objectValue);
  tamper["kind"] = "guard_tamper";
  tamper["cameraName"] = "Patio";
  const CallCopy covered = call_copy::render(
      {.trigger = CallTrigger::GuardCritical, .lang = "es", .data = tamper, .userName = "", .now = kNow});
  CHECK(covered.summary == "La cámara Patio ha dejado de ver.");
  CHECK(covered.missedTitle.starts_with("Llamada perdida · "));

  Json::Value contacts(Json::arrayValue);
  Json::Value vecina(Json::objectValue);
  vecina["name"] = "Vecina";
  vecina["phone"] = "999111222";
  contacts.append(vecina);
  const ResponseNotice unanswered = response_copy::contacts(
      {.lang = "es", .place = "Patio", .contacts = contacts, .emergencyNumber = "105",
       .confirmed = false, .confirmedBy = ""});
  CHECK(unanswered.title == "Nadie ha contestado · Patio");
  CHECK(unanswered.body ==
        "Nadie de casa ha contestado la llamada. Llama a Vecina (999111222), o al 105 si es una emergencia.");
  const Json::Value none(Json::arrayValue);
  const ResponseNotice confirmed = response_copy::contacts(
      {.lang = "en", .place = "", .contacts = none, .emergencyNumber = "",
       .confirmed = true, .confirmedBy = "Pedro"});
  CHECK(confirmed.title == "Confirmed alert");
  CHECK(confirmed.body == "Pedro confirmed it is real.");
  CHECK(response_copy::escalationBody({.lang = "es", .summary = "Hay alguien.",
                                       .reason = ResponseReach::NextStep, .confirmedBy = ""}) ==
        "Hay alguien. Nadie ha contestado todavía.");
  CHECK(response_copy::escalationBody({.lang = "es", .summary = "Hay alguien.",
                                       .reason = ResponseReach::Confirmed, .confirmedBy = "Lucía"}) ==
        "Hay alguien. Lucía ha confirmado que es real.");
  CHECK(response_copy::escalationBody({.lang = "en", .summary = "Someone is there.",
                                       .reason = ResponseReach::Worse, .confirmedBy = ""}) ==
        "Someone is there. It has got worse.");
}

TEST_CASE("a response plan parses defensively")
{
  Json::Value plan(Json::objectValue);
  plan["strategy"] = "bogus";
  plan["stepSeconds"] = 5;
  Json::Value good(Json::objectValue);
  good["userId"] = 3;
  good["step"] = 2;
  good["mode"] = "notify";
  plan["recipients"].append(good);
  plan["recipients"].append(good);
  Json::Value negative(Json::objectValue);
  negative["userId"] = -1;
  negative["step"] = 0;
  plan["recipients"].append(negative);
  Json::Value deep(Json::objectValue);
  deep["userId"] = 4;
  deep["step"] = 99;
  plan["recipients"].append(deep);
  const auto parsed = call_response::parsePlan(plan);
  REQUIRE(parsed.has_value());
  CHECK(parsed->strategy == "ordered");
  CHECK(parsed->stepSeconds == 15);
  REQUIRE(parsed->entries.size() == 1);
  CHECK(parsed->entries.front().mode == ResponseMemberMode::Notify);
  CHECK(parsed->stepCount == 3);
  CHECK_FALSE(call_response::parsePlan(Json::Value(Json::objectValue)));
}

TEST_CASE("a recognised visitor is named in the call, in the user's language")
{
  Json::Value data = guardData({.urgency = "critical", .phase = "opened"});
  data["visitor"]["phraseEs"] = "Es el repartidor que suele venir los martes.";
  data["visitor"]["phraseEn"] = "It is the courier who usually comes on Tuesdays.";
  const auto es = call_copy::render({.trigger = CallTrigger::GuardCritical,
                                     .lang = "es",
                                     .data = data,
                                     .userName = "",
                                     .now = kNow});
  CHECK(es.openingLine.find(" Es el repartidor que suele venir los martes.") !=
        std::string::npos);
  const auto en = call_copy::render({.trigger = CallTrigger::GuardCritical,
                                     .lang = "en",
                                     .data = data,
                                     .userName = "",
                                     .now = kNow});
  CHECK(en.openingLine.find("It is the courier who usually comes on Tuesdays.") !=
        std::string::npos);
  const auto plain = call_copy::render({.trigger = CallTrigger::GuardCritical,
                                        .lang = "es",
                                        .data = guardData({.urgency = "critical", .phase = "opened"}),
                                        .userName = "",
                                        .now = kNow});
  CHECK(plain.openingLine.find("repartidor") == std::string::npos);
}
