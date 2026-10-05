#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <algorithm>
#include <doctest/doctest.h>
#include <errors/validation-exception.hxx>
#include <feature/guard/dtos/update-duty-dto.hxx>
#include <feature/guard/dtos/update-response-dto.hxx>
#include <feature/guard/guard-policy.hxx>
#include <feature/guard/repositories/response/response-repository.hxx>
#include <feature/guard/services/response-plan.hxx>
#include <feature/guard/services/response-verdict-feed.hxx>
#include <text/json-util.hxx>
#include <vector>

using guard_test::GuardBoot;
using guard_test::scalar;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-response-test");
  return shared;
}

int64_t homeId()
{
  return std::stoll(
      scalar("SELECT id FROM guard_environment WHERE is_default = 1"));
}

std::vector<ResponseUser> household()
{
  return {{.userId = 1,
           .role = UserRole::Owner,
           .active = true,
           .name = "Ana",
           .lang = "es"},
          {.userId = 2,
           .role = UserRole::Resident,
           .active = true,
           .name = "Luis",
           .lang = "es"},
          {.userId = 3,
           .role = UserRole::Resident,
           .active = true,
           .name = "Marta",
           .lang = "en"},
          {.userId = 4,
           .role = UserRole::Guard,
           .active = true,
           .name = "Pedro",
           .lang = "es"},
          {.userId = 5,
           .role = UserRole::Guest,
           .active = true,
           .name = "Invitado",
           .lang = "es"},
          {.userId = 6,
           .role = UserRole::Resident,
           .active = false,
           .name = "Baja",
           .lang = "es"}};
}

PresenceRow presenceRow(int64_t userId, PresenceState state,
                        PresenceSource source)
{
  return {.userId = userId,
          .environmentId = 1,
          .state = state,
          .source = source,
          .since = 100,
          .lastHomeAt = state == PresenceState::Home ? 100 : 0,
          .lastSignalAt = 100};
}

struct PlanCase
{
  std::vector<ResponseMember> members;
  std::vector<PresenceRow> presence;
  std::vector<int64_t> excluded;
  ResponseTrigger trigger{ResponseTrigger::Intrusion};
  bool indoor{false};
  bool night{false};
  bool clearThreat{false};
  bool staffedNow{false};
};

ResponsePlan planFor(const PlanCase& input)
{
  return response_plan::build({.environmentId = 1,
                               .members = input.members,
                               .presence = input.presence,
                               .excluded = input.excluded,
                               .trigger = input.trigger,
                               .indoor = input.indoor,
                               .night = input.night,
                               .clearThreat = input.clearThreat,
                               .staffedNow = input.staffedNow,
                               .hasCamera = true,
                               .stepSeconds = 45,
                               .emergencyNumber = "105",
                               .contacts = {}});
}

std::vector<ResponseMember> defaultMembers()
{
  const auto users = household();
  return response_plan::members({.users = users, .rows = {}});
}

const ResponsePlanEntry& entryOf(const ResponsePlan& plan, int64_t userId)
{
  const auto found =
      std::ranges::find(plan.entries, userId, &ResponsePlanEntry::userId);
  REQUIRE(found != plan.entries.end());
  return *found;
}

bool contains(const ResponsePlan& plan, int64_t userId)
{
  return std::ranges::find(plan.entries, userId, &ResponsePlanEntry::userId) !=
         plan.entries.end();
}

}

TEST_CASE("defaults from roles: owner and guard first, residents in order, "
          "guests out")
{
  const auto members = defaultMembers();
  REQUIRE(members.size() == 5);
  const auto stepOf = [&](int64_t userId) {
    return std::ranges::find(members, userId, &ResponseMember::userId)->step;
  };
  const auto modeOf = [&](int64_t userId) {
    return std::ranges::find(members, userId, &ResponseMember::userId)->mode;
  };
  CHECK(stepOf(1) == 0);
  CHECK(stepOf(4) == 0);
  CHECK(stepOf(2) == 1);
  CHECK(stepOf(3) == 2);
  CHECK(modeOf(5) == RecipientMode::Off);
  CHECK(std::ranges::find(members, 6, &ResponseMember::userId) ==
        members.end());
  CHECK(members.front().userId == 1);
  CHECK(members[1].userId == 4);
}

TEST_CASE("owner rows override a user; new residents queue after the last "
          "explicit step")
{
  const auto users = household();
  const std::vector<ResponseRecipientRow> rows{{.environmentId = 1,
                                                .userId = 3,
                                                .mode = RecipientMode::Notify,
                                                .step = 4,
                                                .onDuty = false},
                                               {.environmentId = 1,
                                                .userId = 4,
                                                .mode = std::nullopt,
                                                .step = std::nullopt,
                                                .onDuty = true}};
  const auto members = response_plan::members({.users = users, .rows = rows});
  const auto marta = std::ranges::find(members, 3, &ResponseMember::userId);
  CHECK(marta->mode == RecipientMode::Notify);
  CHECK(marta->step == 4);
  CHECK(marta->customized);
  const auto luis = std::ranges::find(members, 2, &ResponseMember::userId);
  CHECK(luis->step == 5);
  CHECK_FALSE(luis->customized);
  const auto pedro = std::ranges::find(members, 4, &ResponseMember::userId);
  CHECK(pedro->onDuty);
  CHECK(pedro->step == 0);
  CHECK_FALSE(pedro->customized);
}

TEST_CASE("nobody known at home: owner and guards ring together, then "
          "residents one by one")
{
  const auto plan = planFor({.members = defaultMembers(),
                             .presence = {},
                             .excluded = {},
                             .trigger = ResponseTrigger::Intrusion,
                             .indoor = false,
                             .night = false,
                             .clearThreat = false,
                             .staffedNow = false});
  CHECK(plan.strategy == ResponseStrategy::Ordered);
  CHECK(plan.stepCount == 3);
  CHECK(response_plan::stepUsers(plan, 0) == std::vector<int64_t>{1, 4});
  CHECK(response_plan::stepUsers(plan, 1) == std::vector<int64_t>{2});
  CHECK(response_plan::stepUsers(plan, 2) == std::vector<int64_t>{3});
  CHECK_FALSE(contains(plan, 5));
  CHECK_FALSE(plan.offerSiren);
  CHECK(plan.offerCamera);
}

TEST_CASE(
    "people home and the intruder outside: the people inside first, discreetly")
{
  const auto plan =
      planFor({.members = defaultMembers(),
               .presence = {presenceRow(3, PresenceState::Home,
                                        PresenceSource::LanSession),
                            presenceRow(1, PresenceState::Away,
                                        PresenceSource::TunnelSession)},
               .excluded = {},
               .trigger = ResponseTrigger::Intrusion,
               .indoor = false,
               .night = false,
               .clearThreat = false,
               .staffedNow = false});
  CHECK(plan.strategy == ResponseStrategy::InsideFirst);
  CHECK(response_plan::stepUsers(plan, 0) == std::vector<int64_t>{3});
  CHECK(entryOf(plan, 3).discreet);
  CHECK(response_plan::stepUsers(plan, 1) == std::vector<int64_t>{1, 4});
  CHECK_FALSE(entryOf(plan, 1).discreet);
  CHECK(response_plan::stepUsers(plan, 2) == std::vector<int64_t>{2});
  CHECK(plan.stepCount == 3);
  CHECK_FALSE(plan.offerSiren);
}

TEST_CASE("intruder on an indoor camera with people home: everyone at once")
{
  const auto plan = planFor({.members = defaultMembers(),
                             .presence = {presenceRow(2, PresenceState::Home,
                                                      PresenceSource::Camera)},
                             .excluded = {},
                             .trigger = ResponseTrigger::Intrusion,
                             .indoor = true,
                             .night = false,
                             .clearThreat = false,
                             .staffedNow = false});
  CHECK(plan.strategy == ResponseStrategy::Everyone);
  CHECK(plan.stepCount == 1);
  CHECK(response_plan::stepUsers(plan, 0).size() == 4);
  CHECK_FALSE(entryOf(plan, 2).discreet);
}

TEST_CASE("night with everyone home: only a clear threat wakes people")
{
  const std::vector<PresenceRow>
      allHome{presenceRow(1, PresenceState::Home, PresenceSource::LanSession),
              presenceRow(2, PresenceState::Home, PresenceSource::LanSession),
              presenceRow(3, PresenceState::Home, PresenceSource::AppActivity)};
  const auto quiet = planFor({.members = defaultMembers(),
                              .presence = allHome,
                              .excluded = {},
                              .trigger = ResponseTrigger::Intrusion,
                              .indoor = false,
                              .night = true,
                              .clearThreat = false,
                              .staffedNow = false});
  CHECK(quiet.strategy == ResponseStrategy::NightQuiet);
  CHECK(quiet.stepCount == 1);
  CHECK(entryOf(quiet, 1).mode == RecipientMode::Notify);
  CHECK(entryOf(quiet, 2).mode == RecipientMode::Notify);
  CHECK(entryOf(quiet, 4).mode == RecipientMode::Notify);

  const auto threat = planFor({.members = defaultMembers(),
                               .presence = allHome,
                               .excluded = {},
                               .trigger = ResponseTrigger::Intrusion,
                               .indoor = false,
                               .night = true,
                               .clearThreat = true,
                               .staffedNow = false});
  CHECK(threat.strategy == ResponseStrategy::InsideFirst);
  CHECK(entryOf(threat, 1).mode == RecipientMode::Call);

  std::vector<PresenceRow> someoneAway = allHome;
  someoneAway.push_back(
      presenceRow(4, PresenceState::Away, PresenceSource::Timeout));
  const auto partly = planFor({.members = defaultMembers(),
                               .presence = someoneAway,
                               .excluded = {},
                               .trigger = ResponseTrigger::Intrusion,
                               .indoor = false,
                               .night = true,
                               .clearThreat = false,
                               .staffedNow = false});
  CHECK(partly.strategy == ResponseStrategy::InsideFirst);
}

TEST_CASE(
    "a guard on duty is always called, even at night or when listed as notify")
{
  const auto users = household();
  const std::vector<ResponseRecipientRow> rows{{.environmentId = 1,
                                                .userId = 4,
                                                .mode = RecipientMode::Notify,
                                                .step = 0,
                                                .onDuty = true}};
  const auto members = response_plan::members({.users = users, .rows = rows});
  const auto plan = planFor({.members = members,
                             .presence = {},
                             .excluded = {},
                             .trigger = ResponseTrigger::Intrusion,
                             .indoor = false,
                             .night = false,
                             .clearThreat = false,
                             .staffedNow = false});
  CHECK(entryOf(plan, 4).mode == RecipientMode::Call);
  CHECK(entryOf(plan, 4).mandatory);

  const std::vector<PresenceRow> allHome{
      presenceRow(1, PresenceState::Home, PresenceSource::LanSession)};
  const auto night = planFor({.members = members,
                              .presence = allHome,
                              .excluded = {},
                              .trigger = ResponseTrigger::Intrusion,
                              .indoor = false,
                              .night = true,
                              .clearThreat = false,
                              .staffedNow = false});
  CHECK(night.strategy == ResponseStrategy::NightQuiet);
  CHECK(entryOf(night, 4).mode == RecipientMode::Call);

  const std::vector<ResponseRecipientRow> offDuty{
      {.environmentId = 1,
       .userId = 4,
       .mode = RecipientMode::Notify,
       .step = 0,
       .onDuty = false}};
  const auto staffed = planFor(
      {.members = response_plan::members({.users = users, .rows = offDuty}),
       .presence = {},
       .excluded = {},
       .trigger = ResponseTrigger::Intrusion,
       .indoor = false,
       .night = false,
       .clearThreat = false,
       .staffedNow = true});
  CHECK(entryOf(staffed, 4).mandatory);
  const auto unstaffed = planFor(
      {.members = response_plan::members({.users = users, .rows = offDuty}),
       .presence = {},
       .excluded = {},
       .trigger = ResponseTrigger::Intrusion,
       .indoor = false,
       .night = false,
       .clearThreat = false,
       .staffedNow = false});
  CHECK_FALSE(entryOf(unstaffed, 4).mandatory);
  CHECK(entryOf(unstaffed, 4).mode == RecipientMode::Notify);

  const std::vector<ResponseRecipientRow> removed{{.environmentId = 1,
                                                   .userId = 4,
                                                   .mode = RecipientMode::Off,
                                                   .step = 0,
                                                   .onDuty = true}};
  const auto excluded = planFor(
      {.members = response_plan::members({.users = users, .rows = removed}),
       .presence = {},
       .excluded = {},
       .trigger = ResponseTrigger::Intrusion,
       .indoor = false,
       .night = false,
       .clearThreat = false,
       .staffedNow = false});
  CHECK_FALSE(contains(excluded, 4));
}

TEST_CASE("panic and duress reach everyone at once and never the person who "
          "raised it")
{
  for (const ResponseTrigger trigger :
       {ResponseTrigger::Panic, ResponseTrigger::Duress}) {
    const auto plan = planFor({.members = defaultMembers(),
                               .presence = {},
                               .excluded = {2},
                               .trigger = trigger,
                               .indoor = false,
                               .night = false,
                               .clearThreat = false,
                               .staffedNow = false});
    CHECK(plan.strategy == ResponseStrategy::Everyone);
    CHECK_FALSE(contains(plan, 2));
    CHECK(plan.stepCount == 1);
    CHECK(response_plan::stepUsers(plan, 0) == std::vector<int64_t>{1, 4, 3});
  }
  const auto escalation = planFor({.members = defaultMembers(),
                                   .presence = {},
                                   .excluded = {},
                                   .trigger = ResponseTrigger::Escalation,
                                   .indoor = false,
                                   .night = false,
                                   .clearThreat = false,
                                   .staffedNow = false});
  CHECK(escalation.strategy == ResponseStrategy::Everyone);
  const auto tamper =
      planFor({.members = defaultMembers(),
               .presence = {presenceRow(2, PresenceState::Home,
                                        PresenceSource::LanSession)},
               .excluded = {},
               .trigger = ResponseTrigger::Tamper,
               .indoor = false,
               .night = false,
               .clearThreat = false,
               .staffedNow = false});
  CHECK(tamper.strategy == ResponseStrategy::Ordered);
}

TEST_CASE("the siren is offered only when everyone is positively away")
{
  const std::vector<PresenceRow>
      tunnelAway{presenceRow(1, PresenceState::Away,
                             PresenceSource::TunnelSession),
                 presenceRow(2, PresenceState::Away, PresenceSource::Timeout),
                 presenceRow(3, PresenceState::Away, PresenceSource::Timeout),
                 presenceRow(4, PresenceState::Away, PresenceSource::Timeout)};
  CHECK(planFor({.members = defaultMembers(),
                 .presence = tunnelAway,
                 .excluded = {},
                 .trigger = ResponseTrigger::Intrusion,
                 .indoor = false,
                 .night = false,
                 .clearThreat = false,
                 .staffedNow = false})
            .offerSiren);

  std::vector<PresenceRow> timeoutOnly = tunnelAway;
  timeoutOnly.front().source = PresenceSource::Timeout;
  CHECK_FALSE(planFor({.members = defaultMembers(),
                       .presence = timeoutOnly,
                       .excluded = {},
                       .trigger = ResponseTrigger::Intrusion,
                       .indoor = false,
                       .night = false,
                       .clearThreat = false,
                       .staffedNow = false})
                  .offerSiren);

  std::vector<PresenceRow> oneUnknown(tunnelAway.begin(), tunnelAway.end() - 1);
  CHECK_FALSE(planFor({.members = defaultMembers(),
                       .presence = oneUnknown,
                       .excluded = {},
                       .trigger = ResponseTrigger::Intrusion,
                       .indoor = false,
                       .night = false,
                       .clearThreat = false,
                       .staffedNow = false})
                  .offerSiren);
  CHECK_FALSE(planFor({.members = defaultMembers(),
                       .presence = tunnelAway,
                       .excluded = {},
                       .trigger = ResponseTrigger::Panic,
                       .indoor = false,
                       .night = false,
                       .clearThreat = false,
                       .staffedNow = false})
                  .offerSiren);
}

TEST_CASE("the plan travels as JSON with contacts, offers and per-person steps")
{
  const std::vector<ResponseContactRow> contacts{{.id = 1,
                                                  .environmentId = 1,
                                                  .position = 0,
                                                  .name = "Vecina",
                                                  .phone = "+51 999 111 222",
                                                  .note = "Piso 2"}};
  const auto members = defaultMembers();
  const auto plan = response_plan::build({.environmentId = 7,
                                          .members = members,
                                          .presence = {},
                                          .excluded = {},
                                          .trigger = ResponseTrigger::Intrusion,
                                          .indoor = false,
                                          .night = false,
                                          .clearThreat = true,
                                          .staffedNow = false,
                                          .hasCamera = true,
                                          .stepSeconds = 60,
                                          .emergencyNumber = "105",
                                          .contacts = contacts});
  const Json::Value json = response_plan::toJson(plan);
  CHECK(json["v"].asInt() == 1);
  CHECK(json["environmentId"].asInt64() == 7);
  CHECK(json["strategy"].asString() == "ordered");
  CHECK(json["stepCount"].asInt() == 3);
  CHECK(json["stepSeconds"].asInt() == 60);
  CHECK(json["emergencyNumber"].asString() == "105");
  CHECK(json["offers"][0].asString() == "camera");
  CHECK(json["contacts"][0]["phone"].asString() == "+51 999 111 222");
  CHECK(json["recipients"].size() == 4);
  CHECK(json["recipients"][0]["userId"].asInt64() == 1);
  CHECK(json["recipients"][0]["mode"].asString() == "call");
  CHECK(json["recipients"][3]["step"].asInt() == 2);
}

TEST_CASE(
    "the audience of ordinary notices is everyone not off, minus the excluded")
{
  const auto members = defaultMembers();
  const std::vector<int64_t> excluded{4};
  CHECK(response_plan::audience({.members = members, .excluded = excluded}) ==
        std::vector<int64_t>{1, 2, 3});
}

TEST_CASE("an indoor camera with the family home never speaks or sounds")
{
  const GuardDeterrence result =
      guard_policy::deterrence({.mode = GuardMode::Away,
                                .danger = GuardDanger::Critical,
                                .publicPresent = false,
                                .staffOnly = false,
                                .weapon = false,
                                .inAlertZone = true,
                                .encounterChecks = 3,
                                .quietArea = false,
                                .familyInside = true});
  CHECK_FALSE(result.voice);
  CHECK_FALSE(result.alarm);
  const GuardDeterrence outside =
      guard_policy::deterrence({.mode = GuardMode::Away,
                                .danger = GuardDanger::Critical,
                                .publicPresent = false,
                                .staffOnly = false,
                                .weapon = false,
                                .inAlertZone = true,
                                .encounterChecks = 3,
                                .quietArea = false,
                                .familyInside = false});
  CHECK(outside.voice);
  CHECK(outside.alarm);
}

TEST_CASE("the response body is validated field by field")
{
  Json::Value good;
  good["emergencyNumber"] = "105";
  good["stepSeconds"] = 60;
  Json::Value recipient;
  recipient["userId"] = 2;
  recipient["mode"] = "notify";
  recipient["step"] = 1;
  good["recipients"].append(recipient);
  Json::Value contact;
  contact["name"] = " Vecina ";
  contact["phone"] = "+51 (1) 555-0101";
  good["contacts"].append(contact);
  const auto dto = UpdateResponseDto::fromJson(good);
  CHECK(dto.recipients.size() == 1);
  CHECK(dto.contacts.front().name == "Vecina");

  const auto rejects = [](const Json::Value& body) {
    CHECK_THROWS_AS(UpdateResponseDto::fromJson(body), ValidationException);
  };
  Json::Value badPhone = good;
  badPhone["contacts"][0]["phone"] = "call me";
  rejects(badPhone);
  Json::Value twice = good;
  twice["recipients"].append(recipient);
  rejects(twice);
  Json::Value badMode = good;
  badMode["recipients"][0]["mode"] = "siren";
  rejects(badMode);
  Json::Value deepStep = good;
  deepStep["recipients"][0]["step"] = 40;
  rejects(deepStep);
  Json::Value badNumber = good;
  badNumber["emergencyNumber"] = "nine-one-one";
  rejects(badNumber);
  Json::Value fast = good;
  fast["stepSeconds"] = 5;
  rejects(fast);
  Json::Value crowd = good;
  for (int index = 0; index < 11; ++index)
    crowd["contacts"].append(contact);
  rejects(crowd);
  Json::Value missing;
  rejects(missing);

  Json::Value duty;
  duty["onDuty"] = true;
  CHECK(UpdateDutyDto::fromJson(duty).onDuty);
  duty["onDuty"] = "yes";
  CHECK_THROWS_AS(UpdateDutyDto::fromJson(duty), ValidationException);
}

TEST_CASE("the repository replaces a list in one go and keeps duty apart")
{
  (void)boot();
  const ResponseRepository repository;
  const int64_t home = homeId();
  drogon::sync_wait(repository.setDuty(
      {.environmentId = home, .userId = 4, .onDuty = true, .at = 10}));
  auto config = drogon::sync_wait(repository.forEnvironment(home));
  REQUIRE(config.recipients.size() == 1);
  CHECK(config.recipients.front().onDuty);
  CHECK_FALSE(config.recipients.front().mode.has_value());
  CHECK(config.setting.stepSeconds == 45);
  CHECK(config.setting.emergencyNumber.empty());

  drogon::sync_wait(repository.replace(
      {.environmentId = home,
       .emergencyNumber = "105",
       .stepSeconds = 30,
       .recipients = {{.userId = 2,
                       .mode = RecipientMode::Notify,
                       .step = 3,
                       .onDuty = false},
                      {.userId = 4,
                       .mode = RecipientMode::Call,
                       .step = 0,
                       .onDuty = true}},
       .contacts =
           {{.name = "Vecina", .phone = "999111222", .note = "Piso 2"},
            {.name = "Hermano", .phone = "+51 988 000 111", .note = ""}},
       .at = 20}));
  config = drogon::sync_wait(repository.forEnvironment(home));
  REQUIRE(config.recipients.size() == 2);
  CHECK(config.recipients.front().userId == 2);
  CHECK(config.recipients.front().mode == RecipientMode::Notify);
  CHECK(config.recipients.front().step == 3);
  REQUIRE(config.contacts.size() == 2);
  CHECK(config.contacts.front().name == "Vecina");
  CHECK(config.contacts.back().position == 1);
  CHECK(config.setting.emergencyNumber == "105");
  CHECK(config.setting.stepSeconds == 30);

  drogon::sync_wait(repository.setDuty(
      {.environmentId = home, .userId = 4, .onDuty = false, .at = 30}));
  config = drogon::sync_wait(repository.forEnvironment(home));
  const auto pedro =
      std::ranges::find(config.recipients, 4, &ResponseRecipientRow::userId);
  CHECK_FALSE(pedro->onDuty);
  CHECK(pedro->mode == RecipientMode::Call);

  drogon::sync_wait(repository.replace({.environmentId = home,
                                        .emergencyNumber = "",
                                        .stepSeconds = 45,
                                        .recipients = {},
                                        .contacts = {},
                                        .at = 40}));
  config = drogon::sync_wait(repository.forEnvironment(home));
  CHECK(config.recipients.empty());
  CHECK(config.contacts.empty());
  CHECK(drogon::sync_wait(repository.forEnvironment(99999)).recipients.empty());
}

TEST_CASE("a watchlist face is a clear threat even below critical")
{
  Json::Value data(Json::objectValue);
  data["urgency"] = "time_sensitive";
  data["reasons"] = Json::Value(Json::arrayValue);
  data["reasons"].append("night");
  CHECK_FALSE(response_plan::clearThreat(data));
  data["reasons"].append("watchlist");
  CHECK(response_plan::clearThreat(data));
  data["reasons"] = Json::Value(Json::arrayValue);
  data["urgency"] = "critical";
  CHECK(response_plan::clearThreat(data));
}

TEST_CASE("a durable verdict is acknowledged only once the review is stored")
{
  boot();
  scalar("INSERT INTO guard_encounter (first_seen, last_seen) VALUES (1, 2)");
  const std::string episode = scalar("SELECT MAX(id) FROM guard_encounter");
  const ResponseVerdictFeed feed(nullptr);
  const auto verdict = [](const std::string& kind, int64_t episodeId, const std::string& label) {
    Json::Value event(Json::objectValue);
    event["kind"] = kind;
    event["episodeId"] = static_cast<Json::Int64>(episodeId);
    event["verdict"] = label;
    event["at"] = static_cast<Json::Int64>(1'700'000'100);
    return json_util::toString(event);
  };
  CHECK(drogon::sync_wait(feed.apply(verdict("guard_episode", std::stoll(episode), "real"))) ==
        VerdictSettle::Ack);
  CHECK(scalar("SELECT review_label FROM guard_encounter WHERE id = " + episode) == "useful");
  CHECK(drogon::sync_wait(feed.apply(verdict("guard_episode", 999'999, "false_alarm"))) ==
        VerdictSettle::Ack);
  CHECK(drogon::sync_wait(feed.apply(verdict("guard_panic", 0, "real"))) == VerdictSettle::Ack);
  CHECK(drogon::sync_wait(feed.apply("not json")) == VerdictSettle::Discard);
  scalar("CREATE TRIGGER guard_review_refused BEFORE UPDATE OF review_label ON guard_encounter "
         "BEGIN SELECT RAISE(ABORT, 'refused'); END");
  CHECK(drogon::sync_wait(feed.apply(verdict("guard_episode", std::stoll(episode),
                                             "false_alarm"))) == VerdictSettle::Retry);
  scalar("DROP TRIGGER guard_review_refused");
  CHECK(scalar("SELECT review_label FROM guard_encounter WHERE id = " + episode) == "useful");
}
