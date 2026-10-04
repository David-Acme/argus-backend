#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/call/services/call-engine.hxx>
#include <feature/call/services/call-feed.hxx>
#include <feature/call/services/call-preference-service.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr int64_t kStart = 1800000000;

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

struct SharedBoot
{
  std::string path{"call-engine-test-" + std::to_string(::getpid()) + ".db"};
  std::optional<AppRunner> runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = path,
                                   .name = "default",
                                   .timeout = -1});
    runner.emplace();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!drogon::app().isRunning() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!drogon::app().isRunning())
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA))
      throw std::runtime_error("schema did not apply");
  }

  ~SharedBoot()
  {
    runner.reset();
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
  }
};

SharedBoot& boot()
{
  static SharedBoot shared;
  return shared;
}

void reset()
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("DELETE FROM call");
  client->execSqlSync("DELETE FROM call_preference");
  client->execSqlSync("DELETE FROM scheduled_call");
  client->execSqlSync("DELETE FROM call_arrival_seen");
}

class RecordingSignal final : public CallSignal
{
public:
  [[nodiscard]] bool emit(const CallSignalInput& input) const override
  {
    const std::scoped_lock lock(mutex);
    frames.push_back(input);
    return true;
  }

  std::vector<CallSignalInput> of(SyncOperation operation) const
  {
    const std::scoped_lock lock(mutex);
    std::vector<CallSignalInput> found;
    for (const auto& frame : frames) {
      if (frame.operation == operation)
        found.push_back(frame);
    }
    return found;
  }

  mutable std::mutex mutex;
  mutable std::vector<CallSignalInput> frames;
};

class ScriptedAnnouncer final : public LiveCallAnnouncer
{
public:
  [[nodiscard]] std::optional<bool> announce(const CallAnnouncement& input) const override
  {
    const std::scoped_lock lock(mutex);
    heard.push_back(input);
    if (inCall.contains(input.userId))
      return true;
    return reachable ? std::optional<bool>(false) : std::nullopt;
  }

  mutable std::mutex mutex;
  mutable std::vector<CallAnnouncement> heard;
  std::map<int64_t, bool> inCall;
  bool reachable{true};
};

class FixedDirectory final : public CallDirectory
{
public:
  [[nodiscard]] CallRecipient recipient(int64_t userId) const override
  {
    if (userId == 1)
      return {.found = true, .name = "Laura", .lang = "es", .role = "owner", .active = true};
    if (userId == 2)
      return {.found = true, .name = "Tom", .lang = "en", .role = "resident", .active = true};
    if (userId == 3)
      return {.found = true, .name = "Ana", .lang = "es", .role = "resident", .active = false};
    if (userId == 4)
      return {.found = true, .name = "Visita", .lang = "es", .role = "guest", .active = true};
    return {};
  }

  [[nodiscard]] CallPerson person(int64_t personId) const override
  {
    if (personId == 50)
      return {.found = true, .name = "Marta", .userId = 0};
    if (personId == 51)
      return {.found = true, .name = "Tom", .userId = 2};
    return {};
  }
};

class RecordingNotifier final : public CallNotificationSink
{
public:
  drogon::Task<bool> notify(const CallNotice& notice) const override
  {
    const std::scoped_lock lock(mutex);
    notices.push_back(notice);
    co_return true;
  }

  std::vector<CallNotice> all() const
  {
    const std::scoped_lock lock(mutex);
    return notices;
  }

  mutable std::mutex mutex;
  mutable std::vector<CallNotice> notices;
};

class RecordingPush final : public push_intent::PushIntentSink
{
public:
  void publish(const PushIntent& intent) const override
  {
    const std::scoped_lock lock(mutex);
    intents.push_back(intent);
  }

  std::vector<PushIntent> all() const
  {
    const std::scoped_lock lock(mutex);
    return intents;
  }

  mutable std::mutex mutex;
  mutable std::vector<PushIntent> intents;
};

struct Harness
{
  std::shared_ptr<RecordingSignal> signal = std::make_shared<RecordingSignal>();
  std::shared_ptr<ScriptedAnnouncer> announcer =
      std::make_shared<ScriptedAnnouncer>();
  std::shared_ptr<RecordingNotifier> notifier =
      std::make_shared<RecordingNotifier>();
  std::shared_ptr<RecordingPush> push = std::make_shared<RecordingPush>();
  std::shared_ptr<std::atomic<int64_t>> clock =
      std::make_shared<std::atomic<int64_t>>(kStart);
  std::shared_ptr<std::atomic<int>> hour = std::make_shared<std::atomic<int>>(12);
  CallEngine engine;

  Harness()
      : engine(CallEngineConfig{},
               CallEngineDependencies{
                   .signal = signal,
                   .announcer = announcer,
                   .directory = std::make_shared<FixedDirectory>(),
                   .notifier = notifier,
                   .push = push,
                   .clock = [clock = clock]() { return clock->load(); },
                   .localHour = [hour = hour](int64_t) { return hour->load(); },
                   .blockingOffLoop = false})
  {
    reset();
  }

  void advance(int64_t seconds) { clock->fetch_add(seconds); }
};

Json::Value guardData(const std::string& urgency, int64_t episode,
                      const std::string& phase = "opened")
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_episode";
  data["urgency"] = urgency;
  data["phase"] = phase;
  data["threadKey"] = "guard:episode:" + std::to_string(episode);
  data["episodeId"] = static_cast<Json::Int64>(episode);
  data["environmentId"] = 1;
  data["cameraId"] = 6;
  data["cameraName"] = "Patio";
  data["environmentName"] = "";
  data["subject"] = "stranger";
  data["people"] = 1;
  data["action"] = "speaker";
  data["reasons"] = Json::Value(Json::arrayValue);
  data["lang"] = "es";
  return data;
}

Json::Value agendaData(int64_t eventId, int64_t startsAt)
{
  Json::Value data(Json::objectValue);
  data["kind"] = "agenda_event";
  data["threadKey"] =
      "agenda:event:" + std::to_string(eventId) + ":" + std::to_string(startsAt);
  data["title"] = "Dentista";
  data["startsAt"] = static_cast<Json::Int64>(startsAt);
  data["location"] = "";
  return data;
}

std::string stateOf(int64_t callId)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT state FROM call WHERE id = ?", callId);
  return rows.empty() ? std::string{} : rows.front()["state"].as<std::string>();
}

const CallUserOutcome& outcomeFor(const std::vector<CallUserOutcome>& outcomes,
                                  int64_t userId)
{
  for (const auto& outcome : outcomes) {
    if (outcome.userId == userId)
      return outcome;
  }
  throw std::runtime_error("no outcome for user");
}
}

TEST_CASE("a critical guard episode rings every recipient once, in their language")
{
  Harness harness;
  const auto first = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 41), {1, 2, 2, 0}));
  REQUIRE(first.size() == 2);
  CHECK(outcomeFor(first, 1).resolution == CallResolution::Rang);
  CHECK(outcomeFor(first, 2).resolution == CallResolution::Rang);

  const auto frames = harness.signal->of(SyncOperation::CallIncoming);
  REQUIRE(frames.size() == 2);
  const Json::Value& info = frames.front().info;
  CHECK(info["callId"].asString().starts_with("call-"));
  CHECK(info["kind"].asString() == "guard_episode");
  CHECK(info["urgency"].asString() == "critical");
  CHECK(info["episodeId"].asInt64() == 41);
  CHECK(info["cameraId"].asInt64() == 6);
  CHECK(info["cameraName"].asString() == "Patio");
  CHECK(info["expiresAt"].asInt64() == kStart + 45);
  CHECK_FALSE(info.isMember("environmentName"));
  CHECK(frames.back().info["lang"].asString() == "en");

  const auto again = drogon::sync_wait(harness.engine.considerNotification(
      guardData("critical", 41, "escalated"), {1, 2}));
  CHECK(outcomeFor(again, 1).resolution == CallResolution::Dropped);
  CHECK(outcomeFor(again, 1).reason == "already_called");
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 2);
}

TEST_CASE("notifications that are not call-worthy never ring")
{
  Harness harness;
  CHECK(drogon::sync_wait(harness.engine.considerNotification(
                              guardData("active", 42), {1}))
            .empty());
  Json::Value tamper = guardData("critical", 43);
  tamper["kind"] = "guard_tamper";
  CHECK(drogon::sync_wait(harness.engine.considerNotification(tamper, {1})).empty());
  CHECK(harness.signal->frames.empty());
}

TEST_CASE("the first device to answer claims the call; the others are told")
{
  Harness harness;
  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 44), {1}));
  const std::string callId = call_id::format(rang.front().callId);

  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = callId, .userId = 1, .sessionId = "phone"}));
  CHECK(claimed.status == CallClaimStatus::Claimed);
  CHECK(claimed.openingLine.starts_with("Hola, Laura. Te llamo por algo urgente"));
  REQUIRE(claimed.call);
  CHECK(claimed.call.value_or(CallSchema{}).state == CallState::Answered);
  CHECK(stateOf(rang.front().callId) == "answered");

  const auto cancels = harness.signal->of(SyncOperation::CallCancel);
  REQUIRE(cancels.size() == 1);
  CHECK(cancels.front().info["reason"].asString() == "answered_elsewhere");
  CHECK(cancels.front().info["callId"].asString() == callId);

  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = callId, .userId = 1, .sessionId = "desk"}))
            .status == CallClaimStatus::Taken);
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = callId, .userId = 1, .sessionId = "phone"}))
            .status == CallClaimStatus::Claimed);
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = callId, .userId = 2, .sessionId = "phone"}))
            .status == CallClaimStatus::NotFound);
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = "call-999999", .userId = 1, .sessionId = ""}))
            .status == CallClaimStatus::NotFound);
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = "rtc-abc", .userId = 1, .sessionId = ""}))
            .status == CallClaimStatus::NotFound);
}

TEST_CASE("an item that arrives while a call rings joins its opening line")
{
  Harness harness;
  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 45), {1}));
  harness.advance(2);
  const auto queued = drogon::sync_wait(harness.engine.considerNotification(
      agendaData(9, kStart + 600), {1}));
  CHECK(queued.front().resolution == CallResolution::Queued);
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 1);

  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = call_id::format(rang.front().callId),
       .userId = 1,
       .sessionId = "phone"}));
  CHECK(claimed.openingLine.find("Además, a las ") != std::string::npos);
  CHECK(claimed.openingLine.find("«Dentista»") != std::string::npos);
  CHECK(stateOf(queued.front().callId) == "injected");
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = call_id::format(queued.front().callId),
                               .userId = 1,
                               .sessionId = "phone"}))
            .status == CallClaimStatus::NotFound);
}

TEST_CASE("a user already talking to Argus hears the news instead of a ring")
{
  Harness harness;
  harness.announcer->inCall[1] = true;
  const auto outcomes = drogon::sync_wait(
      harness.engine.considerNotification(guardData("time_sensitive", 46), {1, 2}));
  CHECK(outcomeFor(outcomes, 1).resolution == CallResolution::Injected);
  CHECK(outcomeFor(outcomes, 2).resolution == CallResolution::Rang);
  REQUIRE_FALSE(harness.announcer->heard.empty());
  CHECK(harness.announcer->heard.front().text ==
        "Además, hay una persona desconocida en Patio.");
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 1);

  const auto repeat = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 46, "escalated"), {1}));
  CHECK(outcomeFor(repeat, 1).reason == "already_called");
}

TEST_CASE("an unreachable voice service never blocks the ring")
{
  Harness harness;
  harness.announcer->reachable = false;
  const auto outcomes = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 47), {1}));
  CHECK(outcomes.front().resolution == CallResolution::Rang);
}

TEST_CASE("unanswered: a push after the grace, then a missed-call notification")
{
  Harness harness;
  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 48), {1}));
  const int64_t id = rang.front().callId;
  harness.advance(2);
  auto report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.pushed == 0);
  CHECK(harness.push->all().empty());

  harness.advance(3);
  report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.pushed == 1);
  const auto pushes = harness.push->all();
  REQUIRE(pushes.size() == 1);
  CHECK(pushes.front().type == "call");
  CHECK(pushes.front().userId == 1);
  CHECK(pushes.front().data["deepLink"].asString() ==
        "argus://call?callId=" + call_id::format(id));
  CHECK(drogon::sync_wait(harness.engine.sweep()).pushed == 0);

  harness.advance(41);
  report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.missed == 1);
  CHECK(stateOf(id) == "missed");
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().type == "call");
  CHECK(notices.front().title.starts_with("Llamada perdida · "));
  CHECK(notices.front().body ==
        "Te llamé porque había una persona desconocida en Patio.");
  CHECK(notices.front().data["kind"].asString() == "call");
  CHECK(notices.front().data["threadKey"].asString() == "guard:episode:48");
  CHECK(notices.front().data["episodeId"].asInt64() == 48);
  CHECK(notices.front().commandId == "call-missed:" + call_id::format(id));
  const auto cancels = harness.signal->of(SyncOperation::CallCancel);
  REQUIRE(cancels.size() == 1);
  CHECK(cancels.front().info["reason"].asString() == "expired");

  CHECK(drogon::sync_wait(harness.engine.claim({.callId = call_id::format(id),
                                                .userId = 1,
                                                .sessionId = "phone"}))
            .status == CallClaimStatus::Expired);
  CHECK(drogon::sync_wait(harness.engine.sweep()).missed == 0);
}

TEST_CASE("a claim after the ring timed out but before the sweep is expired")
{
  Harness harness;
  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 49), {1}));
  harness.advance(46);
  CHECK(drogon::sync_wait(harness.engine.claim(
                              {.callId = call_id::format(rang.front().callId),
                               .userId = 1,
                               .sessionId = "phone"}))
            .status == CallClaimStatus::Expired);
}

TEST_CASE("a call hung up before Argus spoke counts as declined and leaves a note")
{
  Harness harness;
  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 50), {1, 2}));
  const std::string first = call_id::format(outcomeFor(rang, 1).callId);
  const std::string second = call_id::format(outcomeFor(rang, 2).callId);
  drogon::sync_wait(harness.engine.claim({.callId = first, .userId = 1, .sessionId = "a"}));
  drogon::sync_wait(harness.engine.claim({.callId = second, .userId = 2, .sessionId = "b"}));

  CHECK(drogon::sync_wait(harness.engine.end({.callId = first,
                                              .userId = 1,
                                              .outcome = CallEndReport::Completed,
                                              .spoken = true})));
  CHECK(harness.notifier->all().empty());
  CHECK(stateOf(outcomeFor(rang, 1).callId) == "completed");
  CHECK(drogon::sync_wait(harness.engine.end({.callId = first,
                                              .userId = 1,
                                              .outcome = CallEndReport::Completed,
                                              .spoken = true})));

  CHECK(drogon::sync_wait(harness.engine.end({.callId = second,
                                              .userId = 2,
                                              .outcome = CallEndReport::Declined,
                                              .spoken = false})));
  CHECK(stateOf(outcomeFor(rang, 2).callId) == "declined");
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().userId == 2);
  CHECK(notices.front().title.starts_with("Missed call · "));
  CHECK(notices.front().body == "I called you because there was an unknown person at Patio.");
  CHECK_FALSE(drogon::sync_wait(harness.engine.end({.callId = second,
                                                    .userId = 1,
                                                    .outcome = CallEndReport::Completed,
                                                    .spoken = true})));
}

TEST_CASE("quiet hours, do not disturb and cooldown follow the user's preferences")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto quiet;
  quiet.quietStartHour = 22;
  quiet.quietEndHour = 7;
  const auto saved = drogon::sync_wait(preferences.update(1, quiet));
  CHECK(saved.quietStartHour == 22);
  CHECK(saved.guardCritical == CallMode::Call);

  harness.hour->store(23);
  const auto agenda = drogon::sync_wait(harness.engine.considerNotification(
      agendaData(10, kStart + 600), {1}));
  CHECK(agenda.front().resolution == CallResolution::Notified);
  CHECK(agenda.front().reason == "quiet_hours");
  CHECK(harness.notifier->all().empty());
  const auto critical = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 51), {1}));
  CHECK(critical.front().resolution == CallResolution::Rang);

  harness.hour->store(12);
  harness.advance(60);
  drogon::sync_wait(harness.engine.sweep());
  const auto soon = drogon::sync_wait(harness.engine.considerNotification(
      agendaData(11, kStart + 900), {1}));
  CHECK(soon.front().reason == "cooldown");
  harness.advance(300);
  const auto later = drogon::sync_wait(harness.engine.considerNotification(
      agendaData(12, kStart + 1200), {1}));
  CHECK(later.front().resolution == CallResolution::Rang);

  UpdateCallPreferenceDto dnd;
  dnd.dndUntil = harness.clock->load() + 3600;
  drogon::sync_wait(preferences.update(1, dnd));
  const auto reread = drogon::sync_wait(preferences.read(1));
  CHECK(reread.quietStartHour == 22);
  CHECK(reread.dndUntil == harness.clock->load() + 3600);
  harness.advance(600);
  const auto held = drogon::sync_wait(harness.engine.considerNotification(
      guardData("time_sensitive", 52), {1}));
  CHECK(held.front().reason == "do_not_disturb");
}

TEST_CASE("inactive users are never called")
{
  Harness harness;
  const auto outcomes = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 53), {3}));
  CHECK(outcomes.front().resolution == CallResolution::Dropped);
  CHECK(outcomes.front().reason == "inactive");
}

TEST_CASE("an arrival calls only those who asked, once per absence")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto wants;
  wants.guardArrival = "call";
  drogon::sync_wait(preferences.update(1, wants));
  drogon::sync_wait(preferences.update(2, wants));
  drogon::sync_wait(preferences.update(4, wants));
  UpdateCallPreferenceDto notifyOnly;
  notifyOnly.guardArrival = "notify";
  drogon::sync_wait(preferences.update(5, notifyOnly));

  const KnownSeenEvent event = call_feed::knownSeenFrom([] {
    Json::Value payload(Json::objectValue);
    payload["personId"] = 50;
    payload["cameraId"] = 6;
    payload["cameraName"] = "Entrada";
    payload["environmentId"] = 1;
    payload["environmentName"] = "";
    payload["at"] = static_cast<Json::Int64>(kStart);
    return payload;
  }()).value_or(KnownSeenEvent{});
  REQUIRE(event.personId == 50);
  const auto first = drogon::sync_wait(harness.engine.arrival(event));
  REQUIRE(first.size() == 3);
  CHECK(outcomeFor(first, 1).resolution == CallResolution::Rang);
  CHECK(outcomeFor(first, 2).resolution == CallResolution::Rang);
  CHECK(outcomeFor(first, 5).resolution == CallResolution::Notified);
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().userId == 5);
  CHECK(notices.front().title == "Ha llegado Marta · Entrada");

  KnownSeenEvent soon = event;
  soon.at = kStart + 600;
  CHECK(drogon::sync_wait(harness.engine.arrival(soon)).empty());
  KnownSeenEvent back = event;
  back.at = kStart + 600 + 10800;
  harness.clock->store(back.at);
  CHECK(drogon::sync_wait(harness.engine.arrival(back)).size() == 3);

  KnownSeenEvent tom = event;
  tom.personId = 51;
  const auto own = drogon::sync_wait(harness.engine.arrival(tom));
  CHECK(own.size() == 2);
  for (const auto& outcome : own)
    CHECK(outcome.userId != 2);

  CHECK_FALSE(call_feed::knownSeenFrom(Json::Value(Json::objectValue)));
}

TEST_CASE("scheduled calls: validated, idempotent, rung on time, noted when late")
{
  Harness harness;
  const auto scheduled = drogon::sync_wait(harness.engine.schedule(
      {.userId = 1,
       .fireAt = kStart + 120,
       .topic = "llamar al dentista",
       .lang = "es",
       .commandId = "remind-1"}));
  CHECK(scheduled.status == CallScheduleStatus::Scheduled);
  CHECK(drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                                   .fireAt = kStart + 120,
                                                   .topic = "llamar al dentista",
                                                   .lang = "es",
                                                   .commandId = "remind-1"}))
            .status == CallScheduleStatus::Duplicate);
  CHECK(drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                                   .fireAt = kStart + 180,
                                                   .topic = "otra cosa",
                                                   .lang = "es",
                                                   .commandId = "remind-1"}))
            .status == CallScheduleStatus::Conflict);
  CHECK(drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                                   .fireAt = kStart - 3600,
                                                   .topic = "tarde",
                                                   .lang = "es",
                                                   .commandId = "remind-2"}))
            .status == CallScheduleStatus::Invalid);
  CHECK(drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                                   .fireAt = kStart + 60,
                                                   .topic = "",
                                                   .lang = "es",
                                                   .commandId = "remind-3"}))
            .status == CallScheduleStatus::Invalid);

  harness.advance(60);
  CHECK(drogon::sync_wait(harness.engine.sweep()).fired == 0);
  harness.advance(60);
  const auto report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.fired == 1);
  const auto frames = harness.signal->of(SyncOperation::CallIncoming);
  REQUIRE(frames.size() == 1);
  CHECK(frames.front().info["kind"].asString() == "assistant");
  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = frames.front().info["callId"].asString(),
       .userId = 1,
       .sessionId = "phone"}));
  CHECK(claimed.openingLine ==
        "Hola, Laura. Me pediste que te llamara para recordarte: llamar al dentista.");

  drogon::sync_wait(harness.engine.schedule({.userId = 2,
                                             .fireAt = harness.clock->load() + 10,
                                             .topic = "water the plants",
                                             .lang = "en",
                                             .commandId = "remind-4"}));
  harness.advance(2000);
  drogon::sync_wait(harness.engine.sweep());
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().userId == 2);
  CHECK(notices.front().type == "reminder");
  CHECK(notices.front().body == "water the plants");
}

TEST_CASE("an assistant reminder the user switched off still leaves a note")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto off;
  off.assistant = "off";
  drogon::sync_wait(preferences.update(1, off));
  drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                             .fireAt = kStart + 5,
                                             .topic = "tomar la pastilla",
                                             .lang = "es",
                                             .commandId = "remind-off"}));
  harness.advance(10);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(harness.signal->of(SyncOperation::CallIncoming).empty());
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 1);
  CHECK(notices.front().type == "reminder");
}

TEST_CASE("too many pending scheduled calls are refused")
{
  Harness harness;
  for (int index = 0; index < 20; ++index) {
    CHECK(drogon::sync_wait(harness.engine.schedule(
                                {.userId = 1,
                                 .fireAt = kStart + 3600,
                                 .topic = "t" + std::to_string(index),
                                 .lang = "es",
                                 .commandId = "bulk-" + std::to_string(index)}))
              .status == CallScheduleStatus::Scheduled);
  }
  CHECK(drogon::sync_wait(harness.engine.schedule({.userId = 1,
                                                   .fireAt = kStart + 3600,
                                                   .topic = "one more",
                                                   .lang = "es",
                                                   .commandId = "bulk-x"}))
            .status == CallScheduleStatus::TooMany);
}

TEST_CASE("global switch off turns every call into the notification it was")
{
  Harness harness;
  CallEngineConfig off;
  off.enabled = false;
  harness.engine.reconfigure(off);
  const auto outcomes = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 54), {1}));
  CHECK(outcomes.front().resolution == CallResolution::Notified);
  CHECK(outcomes.front().reason == "calls_disabled");
  CHECK(harness.signal->frames.empty());
}
