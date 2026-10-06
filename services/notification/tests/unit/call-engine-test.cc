#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/call/services/call-engine.hxx>
#include <feature/call/services/call-feed.hxx>
#include <feature/call/services/call-preference-service.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/vocabulary/notification-kind.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <condition_variable>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <unordered_map>
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
  client->execSqlSync("DELETE FROM call_response");
  client->execSqlSync("DELETE FROM call_response_member");
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
      return {.found = true, .name = "Laura", .lang = "es", .role = UserRole::Owner, .active = true};
    if (userId == 2)
      return {.found = true, .name = "Tom", .lang = "en", .role = UserRole::Resident, .active = true};
    if (userId == 3)
      return {.found = true, .name = "Ana", .lang = "es", .role = UserRole::Resident, .active = false};
    if (userId == 4)
      return {.found = true, .name = "Visita", .lang = "es", .role = UserRole::Guest, .active = true};
    if (userId == 5)
      return {.found = true, .name = "Pedro", .lang = "es", .role = UserRole::Guard, .active = true};
    if (userId == 6)
      return {.found = true, .name = "Lucía", .lang = "es", .role = UserRole::Resident, .active = true};
    if (userId == 7)
      return {.found = true, .name = "Futura", .lang = "es", .role = UserRole::Unknown, .active = true};
    return {};
  }

  [[nodiscard]] std::unordered_map<int64_t, CallRecipient>
  recipients(const std::vector<int64_t>& userIds) const override
  {
    batches.fetch_add(1);
    return CallDirectory::recipients(userIds);
  }

  mutable std::atomic<int> batches{0};

  mutable std::atomic<int> failingPersonLookups{0};

  [[nodiscard]] CallPerson person(int64_t personId) const override
  {
    if (failingPersonLookups.load() > 0) {
      failingPersonLookups.fetch_sub(1);
      throw std::runtime_error("identity unreachable");
    }
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

class RecordingVerdicts final : public ResponseVerdictSink
{
public:
  void publish(const ResponseVerdictEvent& event) const override
  {
    const std::scoped_lock lock(mutex);
    events.push_back(event);
  }

  mutable std::mutex mutex;
  mutable std::vector<ResponseVerdictEvent> events;
};

struct Harness
{
  std::shared_ptr<RecordingVerdicts> verdicts = std::make_shared<RecordingVerdicts>();
  std::shared_ptr<RecordingSignal> signal = std::make_shared<RecordingSignal>();
  std::shared_ptr<ScriptedAnnouncer> announcer =
      std::make_shared<ScriptedAnnouncer>();
  std::shared_ptr<RecordingNotifier> notifier =
      std::make_shared<RecordingNotifier>();
  std::shared_ptr<RecordingPush> push = std::make_shared<RecordingPush>();
  std::shared_ptr<std::atomic<int64_t>> clock =
      std::make_shared<std::atomic<int64_t>>(kStart);
  std::shared_ptr<std::atomic<int>> hour = std::make_shared<std::atomic<int>>(12);
  std::shared_ptr<std::atomic<int>> weekday = std::make_shared<std::atomic<int>>(3);
  std::shared_ptr<FixedDirectory> directory = std::make_shared<FixedDirectory>();
  CallEngine engine;

  Harness()
      : engine(CallEngineConfig{},
               CallEngineDependencies{
                   .signal = signal,
                   .announcer = announcer,
                   .directory = directory,
                   .notifier = notifier,
                   .push = push,
                   .verdicts = verdicts,
                   .clock = [clock = clock]() { return clock->load(); },
                   .localTime =
                       [hour = hour, weekday = weekday](int64_t) {
                         return CallLocalTime{.hour = hour->load(),
                                              .weekday = weekday->load()};
                       },
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
  CHECK(harness.directory->batches.load() == 1);

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
  Json::Value tamper = guardData("active", 43);
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
  CHECK(cancels.front().info["claimedBy"].asString() == "phone");
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
  CHECK(pushes.front().title == "Argus te está llamando");
  CHECK(pushes.front().body == "Abre Argus para contestar.");
  CHECK_FALSE(pushes.front().data.isMember("callKind"));
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
  drogon::sync_wait(preferences.update(7, wants));
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
  tom.at = back.at;
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

TEST_CASE("each user's ring length, push delay and language are their own")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto quick;
  quick.ringSeconds = 20;
  quick.pushDelaySeconds = 0;
  quick.lang = "en";
  drogon::sync_wait(preferences.update(1, quick));
  UpdateCallPreferenceDto patient;
  patient.ringSeconds = 90;
  patient.pushDelaySeconds = 30;
  drogon::sync_wait(preferences.update(2, patient));

  const auto rang = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 60), {1, 2}));
  const auto frames = harness.signal->of(SyncOperation::CallIncoming);
  REQUIRE(frames.size() == 2);
  CHECK(frames.front().info["expiresAt"].asInt64() == kStart + 20);
  CHECK(frames.front().info["lang"].asString() == "en");
  CHECK(frames.back().info["expiresAt"].asInt64() == kStart + 90);
  REQUIRE(harness.push->all().size() == 1);
  CHECK(harness.push->all().front().userId == 1);

  harness.advance(21);
  const auto report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.missed == 1);
  CHECK(report.pushed == 0);
  CHECK(stateOf(outcomeFor(rang, 1).callId) == "missed");
  CHECK(stateOf(outcomeFor(rang, 2).callId) == "ringing");
  harness.advance(10);
  CHECK(drogon::sync_wait(harness.engine.sweep()).pushed == 1);
  CHECK(harness.push->all().back().userId == 2);
}

TEST_CASE("a user who keeps calls quiet gets rung, not spoken to")
{
  Harness harness;
  harness.announcer->inCall[1] = true;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto quiet;
  quiet.liveAnnounce = false;
  drogon::sync_wait(preferences.update(1, quiet));
  const auto outcomes = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 61), {1}));
  CHECK(outcomes.front().resolution == CallResolution::Rang);
  CHECK(harness.announcer->heard.empty());
}

TEST_CASE("agenda announcements reach only the users whose lead time matches")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto early;
  early.agendaLeadMinutes = 30;
  drogon::sync_wait(preferences.update(2, early));
  UpdateCallPreferenceDto off;
  off.agenda = "off";
  drogon::sync_wait(preferences.update(3, off));
  UpdateCallPreferenceDto notify;
  notify.agenda = "notify";
  drogon::sync_wait(preferences.update(5, notify));

  const Json::Value data = agendaData(70, kStart + 600);
  const auto tenMinutes = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {1, 2, 3, 5},
       .leadMinutes = 10,
       .title = "Dentista",
       .body = "17:00",
       .data = data,
       .commandId = "agenda:event:70:" + std::to_string(kStart + 600) + ":10"}));
  CHECK(tenMinutes.notified == 2);
  CHECK(tenMinutes.rang == 1);
  const auto notices = harness.notifier->all();
  REQUIRE(notices.size() == 2);
  CHECK(notices.front().userId == 1);
  CHECK(notices.front().type == "agenda");
  CHECK(notices.back().userId == 5);
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 1);

  const auto halfHour = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {1, 2, 3, 5},
       .leadMinutes = 30,
       .title = "Dentista",
       .body = "17:00",
       .data = data,
       .commandId = "agenda:event:70:" + std::to_string(kStart + 600) + ":30"}));
  CHECK(halfHour.notified == 1);
  CHECK(harness.notifier->all().back().userId == 2);

  Json::Value reminder(Json::objectValue);
  reminder["kind"] = "agenda_reminder";
  reminder["threadKey"] = "agenda:reminder:4:" + std::to_string(kStart);
  reminder["title"] = "Sacar la basura";
  const auto due = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {2},
       .leadMinutes = 0,
       .title = "Sacar la basura",
       .body = "",
       .data = reminder,
       .commandId = "agenda:reminder:4:" + std::to_string(kStart) + ":0"}));
  CHECK(due.notified == 1);
}

namespace
{
struct PlanMember
{
  int64_t userId{0};
  int step{0};
  std::string mode{"call"};
  bool mandatory{false};
  bool discreet{false};
};

Json::Value planOf(const std::vector<PlanMember>& members, const std::string& strategy)
{
  Json::Value plan(Json::objectValue);
  plan["v"] = 1;
  plan["environmentId"] = 1;
  plan["strategy"] = strategy;
  plan["stepSeconds"] = 45;
  plan["emergencyNumber"] = "105";
  Json::Value contact(Json::objectValue);
  contact["name"] = "Vecina";
  contact["phone"] = "999111222";
  contact["note"] = "";
  plan["contacts"].append(contact);
  plan["offers"].append("camera");
  int steps = 0;
  for (const auto& member : members) {
    Json::Value item(Json::objectValue);
    item["userId"] = static_cast<Json::Int64>(member.userId);
    item["step"] = member.step;
    item["mode"] = member.mode;
    item["mandatory"] = member.mandatory;
    item["discreet"] = member.discreet;
    plan["recipients"].append(item);
    steps = std::max(steps, member.step + 1);
  }
  plan["stepCount"] = steps;
  return plan;
}

std::vector<PlanMember> orderedHousehold()
{
  return {{.userId = 1, .step = 0, .mode = "call", .mandatory = false, .discreet = false},
          {.userId = 5, .step = 0, .mode = "call", .mandatory = false, .discreet = false},
          {.userId = 2, .step = 1, .mode = "call", .mandatory = false, .discreet = false},
          {.userId = 6, .step = 2, .mode = "call", .mandatory = false, .discreet = false}};
}

struct RespondInput
{
  const Json::Value& data;
  std::vector<int64_t> userIds;
  const Json::Value& plan;
};

std::vector<CallUserOutcome> respond(Harness& harness, const RespondInput& input)
{
  return drogon::sync_wait(harness.engine.respond(
      {.data = input.data, .userIds = input.userIds, .plan = input.plan}));
}

int64_t responseIdOf(const std::string& threadKey)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT id FROM call_response WHERE dedupe_key = ?", threadKey);
  return rows.empty() ? 0 : rows.front()["id"].as<int64_t>();
}

std::string responseState(int64_t id)
{
  const auto rows =
      DbService::client()->execSqlSync("SELECT state FROM call_response WHERE id = ?", id);
  return rows.empty() ? std::string{} : rows.front()["state"].as<std::string>();
}

std::vector<int64_t> ringingUsers()
{
  std::vector<int64_t> users;
  for (const auto& row : DbService::client()->execSqlSync(
           "SELECT user_id FROM call WHERE state = 'ringing' ORDER BY user_id"))
    users.push_back(row["user_id"].as<int64_t>());
  return users;
}

std::vector<CallNotice> noticesFor(const Harness& harness, int64_t userId)
{
  std::vector<CallNotice> found;
  for (const auto& notice : harness.notifier->all()) {
    if (notice.userId == userId)
      found.push_back(notice);
  }
  return found;
}
}

TEST_CASE("a response rings the first step, then escalates step by step, then asks for the contacts")
{
  Harness harness;
  const Json::Value data = guardData("critical", 70);
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  const auto first = respond(harness, {.data = data, .userIds = {1, 5}, .plan = plan});
  CHECK(outcomeFor(first, 1).resolution == CallResolution::Rang);
  CHECK(outcomeFor(first, 5).resolution == CallResolution::Rang);
  CHECK(ringingUsers() == std::vector<int64_t>{1, 5});
  const int64_t id = responseIdOf("guard:episode:70");
  REQUIRE(id > 0);
  CHECK(responseState(id) == "active");
  const auto incoming = harness.signal->of(SyncOperation::CallIncoming);
  REQUIRE(incoming.size() == 2);
  CHECK(incoming.front().info["responseId"].asInt64() == id);
  CHECK(incoming.front().info["offers"][0].asString() == "camera");
  CHECK_FALSE(incoming.front().info["discreet"].asBool());
  const auto updates = harness.signal->of(SyncOperation::ResponseUpdate);
  REQUIRE(updates.size() == 2);
  CHECK(updates.front().info["state"].asString() == "active");
  CHECK(updates.front().info["mine"]["reached"].asBool());

  harness.advance(30);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(ringingUsers() == std::vector<int64_t>{1, 5});

  harness.advance(16);
  const auto second = drogon::sync_wait(harness.engine.sweep());
  CHECK(second.escalated == 1);
  CHECK(second.missed == 2);
  CHECK(ringingUsers() == std::vector<int64_t>{2});
  const auto tom = noticesFor(harness, 2);
  REQUIRE(tom.size() == 1);
  CHECK(tom.front().body.ends_with("Nobody has answered yet."));
  CHECK(tom.front().data["responseId"].asInt64() == id);

  harness.advance(46);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(ringingUsers() == std::vector<int64_t>{6});
  const auto lucia = noticesFor(harness, 6);
  REQUIRE(lucia.size() == 1);
  CHECK(lucia.front().body.ends_with("Nadie ha contestado todavía."));

  harness.advance(46);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(responseState(id) == "unanswered");
  for (const int64_t userId : {1, 5, 2, 6}) {
    const auto notices = noticesFor(harness, userId);
    REQUIRE_FALSE(notices.empty());
    CHECK(notices.back().data["kind"].asString() == "guard_response");
  }
  CHECK(noticesFor(harness, 1).back().body.find("Vecina (999111222)") != std::string::npos);
  CHECK(noticesFor(harness, 1).back().body.find("105") != std::string::npos);
  CHECK(noticesFor(harness, 2).back().title.starts_with("Nobody answered"));
}

TEST_CASE("the first to answer attends: the others stop ringing and see who it is")
{
  Harness harness;
  const Json::Value data = guardData("critical", 71);
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  const auto first = respond(harness, {.data = data, .userIds = {1, 5}, .plan = plan});
  const int64_t id = responseIdOf("guard:episode:71");
  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = call_id::format(outcomeFor(first, 1).callId), .userId = 1, .sessionId = "s1"}));
  CHECK(claimed.status == CallClaimStatus::Claimed);
  CHECK(responseState(id) == "attended");
  CHECK(ringingUsers().empty());
  const auto cancels = harness.signal->of(SyncOperation::CallCancel);
  bool pedroTold = false;
  for (const auto& cancel : cancels) {
    if (cancel.userId == 5) {
      pedroTold = true;
      CHECK(cancel.info["reason"].asString() == "attended");
      CHECK(cancel.info["attendedBy"].asString() == "Laura");
    }
  }
  CHECK(pedroTold);
  const auto updates = harness.signal->of(SyncOperation::ResponseUpdate);
  REQUIRE_FALSE(updates.empty());
  CHECK(updates.back().info["attendedBy"]["name"].asString() == "Laura");
  CHECK(noticesFor(harness, 5).empty());

  harness.advance(200);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(noticesFor(harness, 2).empty());
  CHECK(responseState(id) == "attended");
}

TEST_CASE("a false alarm cancels every ring, stops the escalation and feeds the guard")
{
  Harness harness;
  const Json::Value data = guardData("critical", 72);
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  respond(harness, {.data = data, .userIds = {1, 5}, .plan = plan});
  const int64_t id = responseIdOf("guard:episode:72");

  const auto stranger = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 9, .verdict = ResponseVerdict::FalseAlarm}));
  CHECK(stranger.status == ResponseVerdictStatus::NotFound);
  const auto early = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 2, .verdict = ResponseVerdict::FalseAlarm}));
  CHECK(early.status == ResponseVerdictStatus::NotFound);

  const auto marked = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 5, .verdict = ResponseVerdict::FalseAlarm}));
  CHECK(marked.status == ResponseVerdictStatus::Recorded);
  CHECK(marked.response["state"].asString() == "false_alarm");
  CHECK(marked.response["verdictBy"]["name"].asString() == "Pedro");
  CHECK(marked.response["attendedBy"]["name"].asString() == "Pedro");
  CHECK(ringingUsers().empty());
  REQUIRE(harness.verdicts->events.size() == 1);
  CHECK(harness.verdicts->events.front().episodeId == 72);
  CHECK(harness.verdicts->events.front().verdict == "false_alarm");
  CHECK(harness.verdicts->events.front().kind == "guard_episode");

  const auto again = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 1, .verdict = ResponseVerdict::FalseAlarm}));
  CHECK(again.status == ResponseVerdictStatus::Recorded);
  const auto contrary = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 1, .verdict = ResponseVerdict::Real}));
  CHECK(contrary.status == ResponseVerdictStatus::Closed);
  CHECK(harness.verdicts->events.size() == 1);

  harness.advance(500);
  drogon::sync_wait(harness.engine.sweep());
  CHECK(noticesFor(harness, 2).empty());
  CHECK(respond(harness, {.data = guardData("critical", 72, "escalated"), .userIds = {1, 5, 2, 6},
                          .plan = plan})
            .empty());
}

TEST_CASE("it is real: everyone left is called now and the contacts are offered")
{
  Harness harness;
  const Json::Value data = guardData("time_sensitive", 73);
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  respond(harness, {.data = data, .userIds = {1, 5}, .plan = plan});
  const int64_t id = responseIdOf("guard:episode:73");
  const auto confirmed = drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 1, .verdict = ResponseVerdict::Real}));
  CHECK(confirmed.status == ResponseVerdictStatus::Recorded);
  CHECK(responseState(id) == "confirmed");
  CHECK(confirmed.response["showContacts"].asBool());
  CHECK(confirmed.response["emergencyNumber"].asString() == "105");
  CHECK(ringingUsers() == std::vector<int64_t>{2, 5, 6});
  bool lauraStopped = false;
  for (const auto& cancel : harness.signal->of(SyncOperation::CallCancel))
    lauraStopped = lauraStopped || (cancel.userId == 1 && cancel.info["reason"].asString() == "attended");
  CHECK(lauraStopped);
  const auto tomReached = noticesFor(harness, 2);
  REQUIRE(tomReached.size() >= 2);
  CHECK(tomReached.front().body.ends_with("Laura confirmed it is real."));
  const auto incoming = harness.signal->of(SyncOperation::CallIncoming);
  bool tomCritical = false;
  for (const auto& frame : incoming) {
    if (frame.userId == 2)
      tomCritical = frame.info["urgency"].asString() == "critical";
  }
  CHECK(tomCritical);
  CHECK(noticesFor(harness, 6).back().title.starts_with("Alerta confirmada"));
  CHECK(noticesFor(harness, 6).back().body.find("Laura ha confirmado") != std::string::npos);
  CHECK(harness.verdicts->events.front().verdict == "real");
}

TEST_CASE("a guard on duty rings despite turning intruder calls off; a notify member only reads")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto off;
  off.guardIntruder = "off";
  off.enabled = false;
  drogon::sync_wait(preferences.update(5, off));
  drogon::sync_wait(preferences.update(1, off));
  const Json::Value plan =
      planOf({{.userId = 5, .step = 0, .mode = "call", .mandatory = true, .discreet = false},
              {.userId = 1, .step = 0, .mode = "call", .mandatory = false, .discreet = false},
              {.userId = 6, .step = 0, .mode = "notify", .mandatory = false, .discreet = false}},
             "ordered");
  const auto outcomes =
      respond(harness, {.data = guardData("time_sensitive", 74), .userIds = {5, 1, 6}, .plan = plan});
  CHECK(outcomeFor(outcomes, 5).resolution == CallResolution::Rang);
  CHECK(outcomeFor(outcomes, 5).reason == "on_duty");
  CHECK(outcomeFor(outcomes, 1).resolution == CallResolution::Dropped);
  CHECK(outcomeFor(outcomes, 6).resolution == CallResolution::Notified);
  CHECK(outcomeFor(outcomes, 6).reason == "plan_notify");
}

TEST_CASE("the people inside are warned quietly")
{
  Harness harness;
  const Json::Value plan =
      planOf({{.userId = 6, .step = 0, .mode = "call", .mandatory = false, .discreet = true},
              {.userId = 1, .step = 1, .mode = "call", .mandatory = false, .discreet = false}},
             "inside_first");
  const auto outcomes =
      respond(harness, {.data = guardData("time_sensitive", 75), .userIds = {6}, .plan = plan});
  CHECK(outcomeFor(outcomes, 6).resolution == CallResolution::Rang);
  const auto incoming = harness.signal->of(SyncOperation::CallIncoming);
  REQUIRE(incoming.size() == 1);
  CHECK(incoming.front().info["discreet"].asBool());
  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = call_id::format(outcomeFor(outcomes, 6).callId), .userId = 6, .sessionId = "s6"}));
  CHECK(claimed.openingLine.find("en voz baja") != std::string::npos);
  CHECK(claimed.openingLine.find("No abras") != std::string::npos);
}

TEST_CASE("panic and duress never reach the person who raised them")
{
  Harness harness;
  Json::Value panic(Json::objectValue);
  panic["kind"] = "guard_panic";
  panic["urgency"] = "critical";
  panic["phase"] = "opened";
  panic["threadKey"] = "guard:panic:9";
  panic["episodeId"] = 9;
  panic["incidentId"] = 9;
  panic["environmentId"] = 1;
  panic["environmentName"] = "Casa";
  panic["cameraId"] = 0;
  panic["actorUserId"] = 2;
  const Json::Value plan =
      planOf({{.userId = 1, .step = 0, .mode = "call", .mandatory = false, .discreet = false},
              {.userId = 2, .step = 0, .mode = "call", .mandatory = false, .discreet = false},
              {.userId = 6, .step = 0, .mode = "call", .mandatory = false, .discreet = false}},
             "everyone");
  const auto outcomes = respond(harness, {.data = panic, .userIds = {1, 2, 6}, .plan = plan});
  CHECK(outcomes.size() == 2);
  CHECK(ringingUsers() == std::vector<int64_t>{1, 6});
  for (const auto& frame : harness.signal->frames)
    CHECK(frame.userId != 2);
  const int64_t id = responseIdOf("guard:panic:9");
  CHECK_FALSE(drogon::sync_wait(harness.engine.response({.userId = 2, .responseId = id})));
  const auto incoming = harness.signal->of(SyncOperation::CallIncoming);
  CHECK(incoming.front().info["reason"].asString() == "Botón de pánico · Casa");
  const auto claimed = drogon::sync_wait(harness.engine.claim(
      {.callId = call_id::format(outcomeFor(outcomes, 1).callId), .userId = 1, .sessionId = "s1"}));
  CHECK(claimed.openingLine.find("Tom ha pulsado el botón de pánico") != std::string::npos);
  drogon::sync_wait(harness.engine.verdict(
      {.responseId = id, .userId = 1, .verdict = ResponseVerdict::FalseAlarm}));
  CHECK(harness.verdicts->events.front().kind == "guard_panic");
  CHECK(harness.verdicts->events.front().episodeId == 0);
}

TEST_CASE("a worse turn of the same episode reaches everyone left at once")
{
  Harness harness;
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  respond(harness, {.data = guardData("time_sensitive", 76), .userIds = {1, 5}, .plan = plan});
  CHECK(ringingUsers() == std::vector<int64_t>{1, 5});
  respond(harness, {.data = guardData("critical", 76, "escalated"), .userIds = {1, 5, 2, 6},
                    .plan = planOf(orderedHousehold(), "everyone")});
  CHECK(ringingUsers() == std::vector<int64_t>{1, 2, 5, 6});
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 4);
}

TEST_CASE("each person lists only the responses that reached them")
{
  Harness harness;
  const Json::Value plan = planOf(orderedHousehold(), "ordered");
  respond(harness, {.data = guardData("critical", 77), .userIds = {1, 5}, .plan = plan});
  const int64_t id = responseIdOf("guard:episode:77");
  const Json::Value mine = drogon::sync_wait(harness.engine.responses(1));
  REQUIRE(mine.size() == 1);
  CHECK(mine[0]["id"].asInt64() == id);
  CHECK(mine[0]["contacts"][0]["name"].asString() == "Vecina");
  CHECK_FALSE(mine[0]["showContacts"].asBool());
  CHECK(drogon::sync_wait(harness.engine.responses(2)).empty());
  CHECK_FALSE(drogon::sync_wait(harness.engine.response({.userId = 2, .responseId = id})));
  CHECK(drogon::sync_wait(harness.engine.response({.userId = 5, .responseId = id})));
}

TEST_CASE("a plan that does not parse falls back to the plain call")
{
  Harness harness;
  Json::Value broken(Json::objectValue);
  broken["recipients"] = "everyone";
  const auto outcomes =
      respond(harness, {.data = guardData("critical", 78), .userIds = {1, 2}, .plan = broken});
  CHECK(outcomes.size() == 2);
  CHECK(responseIdOf("guard:episode:78") == 0);
  CHECK(harness.signal->of(SyncOperation::ResponseUpdate).empty());
}

TEST_CASE("an arrival whose first attempt fails still calls on the durable retry")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto wants;
  wants.guardArrival = "call";
  drogon::sync_wait(preferences.update(1, wants));
  const KnownSeenEvent event{.personId = 50, .cameraId = 6, .cameraName = "Entrada",
                             .environmentId = 1, .environmentName = "", .at = kStart};

  harness.directory->failingPersonLookups.store(1);
  CHECK_THROWS_AS(drogon::sync_wait(harness.engine.arrival(event)), std::runtime_error);
  CHECK(DbService::client()
            ->execSqlSync("SELECT last_seen FROM call_arrival_seen WHERE person_id = 50")
            .empty());

  const auto retried = drogon::sync_wait(harness.engine.arrival(event));
  REQUIRE(retried.size() == 1);
  CHECK(outcomeFor(retried, 1).resolution == CallResolution::Rang);

  CHECK(drogon::sync_wait(harness.engine.arrival(event)).empty());
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 1);
}

TEST_CASE("an arrival replayed long after it happened only updates the absence")
{
  Harness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto wants;
  wants.guardArrival = "call";
  drogon::sync_wait(preferences.update(1, wants));
  harness.clock->store(kStart + call_engine::kArrivalStaleS + 1);
  const auto replayed = drogon::sync_wait(harness.engine.arrival(
      {.personId = 50, .cameraId = 6, .cameraName = "Entrada",
       .environmentId = 1, .environmentName = "", .at = kStart}));
  CHECK(replayed.empty());
  CHECK(harness.signal->frames.empty());
  const auto rows = DbService::client()->execSqlSync(
      "SELECT last_seen FROM call_arrival_seen WHERE person_id = 50");
  REQUIRE(rows.size() == 1);
  CHECK(rows.front()["last_seen"].as<int64_t>() == kStart);
}

namespace
{
class GatheringAnnouncer final : public LiveCallAnnouncer
{
public:
  explicit GatheringAnnouncer(int expected) : expected_(expected) {}

  [[nodiscard]] std::optional<bool> announce(const CallAnnouncement& input) const override
  {
    std::unique_lock lock(mutex_);
    ++inside_;
    peak = std::max(peak, inside_);
    if (inside_ >= expected_)
      gathered_ = true;
    arrived_.notify_all();
    arrived_.wait_for(lock, std::chrono::seconds(5), [this] { return gathered_; });
    --inside_;
    return input.userId == 2;
  }

  mutable int peak{0};

private:
  int expected_{0};
  mutable int inside_{0};
  mutable bool gathered_{false};
  mutable std::mutex mutex_;
  mutable std::condition_variable arrived_;
};
}

TEST_CASE("live-call probes for several recipients run side by side")
{
  const GatheringAnnouncer announcer(3);
  const std::vector<CallAnnouncement> probes{
      {.userId = 1, .text = "a", .kind = "guard_episode", .callId = ""},
      {.userId = 2, .text = "b", .kind = "guard_episode", .callId = ""},
      {.userId = 6, .text = "c", .kind = "guard_episode", .callId = ""}};
  const auto heard = call_engine::announceAll(
      {.announcer = announcer, .probes = probes, .parallel = 3});
  REQUIRE(heard.size() == 3);
  CHECK(heard[0] == std::optional<bool>(false));
  CHECK(heard[1] == std::optional<bool>(true));
  CHECK(heard[2] == std::optional<bool>(false));
  CHECK(announcer.peak == 3);
}

TEST_CASE("the sweep purges call history past the retention window")
{
  Harness harness;
  const int64_t old = kStart - (int64_t{31} * 86400);
  const int64_t recent = kStart - 86400;
  const auto client = DbService::client();
  client->execSqlSync(
      "INSERT INTO call (user_id, dedupe_key, trigger, state, created_at) "
      "VALUES (1, 'old', 'agenda', 'missed', ?), "
      "(1, 'recent', 'agenda', 'missed', ?)",
      old, recent);
  client->execSqlSync(
      "INSERT INTO scheduled_call (user_id, command_id, fire_at, topic, state, "
      "created_at) VALUES (1, 'fired-old', ?, 't', 'fired', ?), "
      "(1, 'pending-old', ?, 't', 'pending', ?)",
      old, old, kStart + (int64_t{10} * 86400), old);
  client->execSqlSync(
      "INSERT INTO call_arrival_seen (person_id, last_seen) VALUES (90, ?), (91, ?)",
      old, recent);
  client->execSqlSync(
      "INSERT INTO call_response (id, dedupe_key, kind, state, created_at, "
      "updated_at) VALUES (900, 'guard:episode:900', 'guard_episode', "
      "'false_alarm', ?, ?), (901, 'guard:episode:901', 'guard_episode', "
      "'false_alarm', ?, ?)",
      old, old, recent, recent);
  client->execSqlSync(
      "INSERT INTO call_response_member (response_id, user_id) VALUES "
      "(900, 1), (901, 1)");

  const auto report = drogon::sync_wait(harness.engine.sweep());
  CHECK(report.purged == 4);
  const auto count = [&client](const char* sql) {
    return client->execSqlSync(sql).front()["total"].as<int64_t>();
  };
  CHECK(count("SELECT COUNT(*) AS total FROM call") == 1);
  CHECK(count("SELECT COUNT(*) AS total FROM scheduled_call") == 1);
  CHECK(count("SELECT COUNT(*) AS total FROM call_arrival_seen") == 1);
  CHECK(count("SELECT COUNT(*) AS total FROM call_response") == 1);
  CHECK(count("SELECT COUNT(*) AS total FROM call_response_member") == 1);

  client->execSqlSync(
      "INSERT INTO call_arrival_seen (person_id, last_seen) VALUES (92, ?)", old);
  harness.advance(60);
  CHECK(drogon::sync_wait(harness.engine.sweep()).purged == 0);
  harness.advance(call_engine::kPurgeIntervalS);
  CHECK(drogon::sync_wait(harness.engine.sweep()).purged == 1);
}

namespace
{
struct ModuleSwitches
{
  std::shared_ptr<std::atomic<bool>> surveillance = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<bool>> productivity = std::make_shared<std::atomic<bool>>(true);

  [[nodiscard]] std::function<bool(std::string_view)> predicate() const
  {
    return [surveillance = surveillance, productivity = productivity](std::string_view kind) {
      const auto module = notification_kind::moduleOf(kind);
      if (module == notification_kind::kSurveillance)
        return surveillance->load();
      if (module == notification_kind::kProductivity)
        return productivity->load();
      return true;
    };
  }
};

struct GatedHarness
{
  ModuleSwitches modules;
  std::shared_ptr<RecordingSignal> signal = std::make_shared<RecordingSignal>();
  std::shared_ptr<RecordingNotifier> notifier = std::make_shared<RecordingNotifier>();
  std::shared_ptr<FixedDirectory> directory = std::make_shared<FixedDirectory>();
  std::shared_ptr<std::atomic<int64_t>> clock = std::make_shared<std::atomic<int64_t>>(kStart);
  CallEngine engine;

  GatedHarness()
      : engine(CallEngineConfig{},
               CallEngineDependencies{.signal = signal,
                                      .announcer = std::make_shared<ScriptedAnnouncer>(),
                                      .directory = directory,
                                      .notifier = notifier,
                                      .push = std::make_shared<RecordingPush>(),
                                      .verdicts = std::make_shared<RecordingVerdicts>(),
                                      .clock = [clock = clock]() { return clock->load(); },
                                      .localTime = [](int64_t) { return CallLocalTime{.hour = 12, .weekday = 3}; },
                                      .blockingOffLoop = false,
                                      .kindAllowed = modules.predicate()})
  {
    reset();
  }
};

Json::Value panicData(int64_t alert)
{
  Json::Value data = guardData("critical", alert);
  data["kind"] = "guard_panic";
  data["threadKey"] = "guard:panic:" + std::to_string(alert);
  return data;
}
}

TEST_CASE("the notification kinds of a module name that module and nothing else does")
{
  CHECK(notification_kind::moduleOf("guard_episode") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("guard_tamper") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("guard_digest") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("guard_arrival") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("camera_fallback") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("camera_fallback_digest") == notification_kind::kSurveillance);
  CHECK(notification_kind::moduleOf("agenda_event") == notification_kind::kProductivity);
  for (const char* core : {"guard_panic", "guard_duress", "guard_panic_sent", "guard_response", "agenda_reminder",
                           "assistant_reminder", "call", "module_request", "system", ""})
    CHECK(notification_kind::moduleOf(core).empty());
}

TEST_CASE("an agenda event is not announced while productivity is off, a reminder still is")
{
  GatedHarness harness;
  harness.modules.productivity->store(false);

  const Json::Value event = agendaData(80, kStart + 600);
  const auto skipped = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {1, 2},
       .leadMinutes = 10,
       .title = "Dentista",
       .body = "17:00",
       .data = event,
       .commandId = "agenda:event:80:" + std::to_string(kStart + 600) + ":10"}));
  CHECK(skipped.notified == 0);
  CHECK(skipped.rang == 0);
  CHECK(harness.notifier->all().empty());
  CHECK(harness.signal->of(SyncOperation::CallIncoming).empty());

  Json::Value reminder(Json::objectValue);
  reminder["kind"] = "agenda_reminder";
  reminder["threadKey"] = "agenda:reminder:8:" + std::to_string(kStart);
  reminder["title"] = "Sacar la basura";
  const auto due = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {1},
       .leadMinutes = 0,
       .title = "Sacar la basura",
       .body = "",
       .data = reminder,
       .commandId = "agenda:reminder:8:" + std::to_string(kStart) + ":0"}));
  CHECK(due.notified == 1);
  CHECK(due.rang == 1);

  harness.modules.productivity->store(true);
  const auto back = drogon::sync_wait(harness.engine.announceAgenda(
      {.userIds = {1, 2},
       .leadMinutes = 10,
       .title = "Dentista",
       .body = "17:00",
       .data = event,
       .commandId = "agenda:event:80:" + std::to_string(kStart + 600) + ":10"}));
  CHECK(back.notified == 2);
}

TEST_CASE("an arrival does not call while surveillance is off")
{
  GatedHarness harness;
  const CallPreferenceService preferences;
  UpdateCallPreferenceDto wants;
  wants.guardArrival = "call";
  drogon::sync_wait(preferences.update(1, wants));

  KnownSeenEvent event;
  event.personId = 50;
  event.cameraId = 6;
  event.cameraName = "Entrada";
  event.environmentId = 1;
  event.at = kStart;

  harness.modules.surveillance->store(false);
  CHECK(drogon::sync_wait(harness.engine.arrival(event)).empty());
  CHECK(harness.signal->of(SyncOperation::CallIncoming).empty());

  harness.modules.surveillance->store(true);
  event.at = kStart + 10800 + 600;
  harness.clock->store(event.at);
  const auto back = drogon::sync_wait(harness.engine.arrival(event));
  CHECK(back.size() == 1);
  CHECK(harness.signal->of(SyncOperation::CallIncoming).size() == 1);
}

TEST_CASE("turning a module off ends the rings of its kinds and leaves every other ring")
{
  GatedHarness harness;
  const auto episode = drogon::sync_wait(
      harness.engine.considerNotification(guardData("critical", 90), {1}));
  const auto panic = drogon::sync_wait(
      harness.engine.considerNotification(panicData(91), {2}));
  const auto agenda = drogon::sync_wait(
      harness.engine.considerNotification(agendaData(92, kStart + 300), {5}));
  REQUIRE(episode.size() == 1);
  REQUIRE(panic.size() == 1);
  REQUIRE(agenda.size() == 1);
  CHECK(outcomeFor(episode, 1).resolution == CallResolution::Rang);
  CHECK(outcomeFor(panic, 2).resolution == CallResolution::Rang);
  CHECK(outcomeFor(agenda, 5).resolution == CallResolution::Rang);
  const int64_t episodeCall = outcomeFor(episode, 1).callId;
  const int64_t panicCall = outcomeFor(panic, 2).callId;
  const int64_t agendaCall = outcomeFor(agenda, 5).callId;

  CHECK(drogon::sync_wait(harness.engine.cancelForModule("agronomy")) == 0);
  CHECK(drogon::sync_wait(harness.engine.cancelForModule("surveillance")) == 1);
  CHECK(stateOf(episodeCall) == "missed");
  CHECK(stateOf(panicCall) == "ringing");
  CHECK(stateOf(agendaCall) == "ringing");
  const auto cancels = harness.signal->of(SyncOperation::CallCancel);
  REQUIRE(cancels.size() == 1);
  CHECK(cancels.front().userId == 1);
  CHECK(cancels.front().info["reason"].asString() == "module_disabled");
  CHECK(harness.notifier->all().empty());
  CHECK(drogon::sync_wait(harness.engine.cancelForModule("surveillance")) == 0);

  CHECK(drogon::sync_wait(harness.engine.cancelForModule("productivity")) == 1);
  CHECK(stateOf(agendaCall) == "missed");
  CHECK(stateOf(panicCall) == "ringing");
  CHECK(harness.notifier->all().empty());
}

TEST_CASE("the notification funnel drops the kinds of a module that is off and keeps the rest")
{
  boot();
  const ModuleSwitches modules;
  const NotificationService service({.deliverySink = nullptr,
                                     .pushSink = nullptr,
                                     .pushRequired = false,
                                     .kindAllowed = modules.predicate()});
  const auto count = [] {
    const auto rows = DbService::client()->execSqlSync("SELECT COUNT(*) AS total FROM notification");
    return rows.front()["total"].as<int64_t>();
  };
  const auto before = count();
  const auto batchOf = [](const std::string& kind, const std::string& command) {
    NotificationBatchInput batch;
    batch.userIds = {1, 2};
    batch.notification.type = "camera";
    batch.notification.title = kind;
    batch.notification.body = "body";
    batch.notification.data = Json::Value(Json::objectValue);
    batch.notification.data["kind"] = kind;
    batch.commandId = command;
    return batch;
  };

  modules.surveillance->store(false);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("guard_episode", "gate:episode:off"))).createdCount == 0);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("camera_fallback", "gate:fallback:off"))).createdCount == 0);
  CHECK(count() == before);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("guard_panic", "gate:panic:off"))).createdCount == 2);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("agenda_event", "gate:agenda:on"))).createdCount == 2);
  CHECK(count() == before + 4);

  modules.surveillance->store(true);
  modules.productivity->store(false);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("guard_episode", "gate:episode:on"))).createdCount == 2);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("agenda_event", "gate:agenda:off"))).createdCount == 0);
  CHECK(drogon::sync_wait(service.createManyAndEmit(batchOf("agenda_reminder", "gate:reminder:off"))).createdCount == 2);
  CHECK(count() == before + 8);
}
