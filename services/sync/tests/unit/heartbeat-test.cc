#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/heartbeat/infra/guard-presence-directory.hxx>
#include <feature/heartbeat/services/heartbeat-feed.hxx>
#include <feature/heartbeat/services/heartbeat-policy.hxx>
#include <feature/heartbeat/services/heartbeat-service.hxx>
#include <feature/heartbeat/services/presence-board.hxx>
#include <text/json-util.hxx>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
const HeartbeatPolicy kPolicy{.intervalSeconds = 60,
                              .graceSeconds = 2700,
                              .socketGraceSeconds = 180,
                              .guardStaleSeconds = 90};

class RecordingPush : public push_intent::PushIntentSink
{
public:
  void publish(const PushIntent& intent) const override { sent.push_back(intent); }

  mutable std::vector<PushIntent> sent;
};

class FakeDirectory : public PresenceDirectory
{
public:
  [[nodiscard]] std::optional<std::vector<PresenceEntry>> list() const override
  {
    if (duringList)
      duringList();
    return answer;
  }

  std::optional<std::vector<PresenceEntry>> answer;
  std::function<void()> duringList;
};

struct Emitted
{
  int64_t userId{0};
  Json::Value frame;
};

struct FeedRig
{
  std::shared_ptr<PresenceBoard> board = std::make_shared<PresenceBoard>();
  std::shared_ptr<const HeartbeatService> service =
      std::make_shared<const HeartbeatService>(
          HeartbeatService::Dependencies{.board = board, .clock = [] { return int64_t{5000}; }},
          kPolicy);
  std::shared_ptr<FakeDirectory> directory = std::make_shared<FakeDirectory>();
  RecordingPush push;
  std::vector<Emitted> emitted;
  HeartbeatFeed feed{
      HeartbeatFeed::Dependencies{
          .bus = nullptr,
          .board = board,
          .heartbeat = service,
          .directory = directory,
          .push = &push,
          .emit = [this](int64_t userId, std::string_view frame) {
            emitted.push_back({.userId = userId,
                               .frame = json_util::fromString(std::string(frame))});
          }},
      HeartbeatFeed::Config{.presenceSubject = {},
                            .guardHeartbeatSubject = {},
                            .pushIntervalSeconds = 0,
                            .refillSeconds = 0}};
};
}

TEST_CASE("only an away user is armed; home, unknown and garbage never arm")
{
  CHECK(heartbeat::armed("away"));
  CHECK_FALSE(heartbeat::armed("home"));
  CHECK_FALSE(heartbeat::armed("unknown"));
  CHECK(heartbeat::normalizePresence("AWAY") == "unknown");
  CHECK(heartbeat::normalizePresence("") == "unknown");

  const Json::Value away = heartbeat::render(
      {.now = 1000, .presence = "away", .presenceSince = 900, .guardSeenAt = 990}, kPolicy);
  CHECK(away["armed"].asBool());
  CHECK(away["presence"].asString() == "away");
  CHECK(away["presenceSince"].asInt64() == 900);
  CHECK(away["at"].asInt64() == 1000);
  CHECK(away["graceSeconds"].asInt64() == 2700);
  CHECK(away["socketGraceSeconds"].asInt64() == 180);
  CHECK(away["intervalSeconds"].asInt64() == 60);
  CHECK(away["guard"].asString() == "alive");

  const Json::Value unknown = heartbeat::render(
      {.now = 1000, .presence = "maybe", .presenceSince = 900, .guardSeenAt = 0}, kPolicy);
  CHECK_FALSE(unknown["armed"].asBool());
  CHECK(unknown["presence"].asString() == "unknown");
  CHECK(unknown["presenceSince"].asInt64() == 0);
  CHECK(unknown["guard"].asString() == "unknown");
}

TEST_CASE("guard is stale once its heartbeat is older than the stale window")
{
  CHECK(heartbeat::guardState({.now = 1000, .presence = {}, .presenceSince = 0, .guardSeenAt = 910},
                              kPolicy) == "alive");
  CHECK(heartbeat::guardState({.now = 1000, .presence = {}, .presenceSince = 0, .guardSeenAt = 909},
                              kPolicy) == "stale");
}

TEST_CASE("the board keeps one entry per user and reports changes only")
{
  PresenceBoard board;
  CHECK(board.of(7).overall == "unknown");
  CHECK(board.apply({.userId = 7, .overall = "away", .since = 100}));
  CHECK_FALSE(board.apply({.userId = 7, .overall = "away", .since = 100}));
  CHECK(board.apply({.userId = 8, .overall = "home", .since = 50}));
  CHECK(board.apply({.userId = 9, .overall = "away", .since = 60}));
  CHECK_FALSE(board.apply({.userId = 0, .overall = "away", .since = 60}));
  CHECK(board.armedUsers() == std::vector<int64_t>{7, 9});
  CHECK(board.apply({.userId = 7, .overall = "home", .since = 200}));
  CHECK(board.armedUsers() == std::vector<int64_t>{9});

  board.markGuardSeen(500);
  board.markGuardSeen(400);
  CHECK(board.guardSeenAt() == 500);
}

TEST_CASE("presence payloads parse strictly")
{
  const auto parsed = heartbeat::parsePresence(
      R"({"userId":4,"environmentId":1,"state":"away","source":"timeout","since":77,"overall":"away"})");
  REQUIRE(parsed.has_value());
  const PresenceEntry entry = parsed.value_or(PresenceEntry{});
  CHECK(entry.userId == 4);
  CHECK(entry.overall == "away");
  CHECK(entry.since == 77);
  CHECK_FALSE(heartbeat::parsePresence("not json").has_value());
  CHECK_FALSE(heartbeat::parsePresence(R"({"userId":0,"overall":"away"})").has_value());
  CHECK_FALSE(heartbeat::parsePresence(R"({"userId":3})").has_value());
  CHECK(heartbeat::parsePresence(R"({"userId":3,"overall":"elsewhere"})")
            .value_or(PresenceEntry{.userId = 0, .overall = "x", .since = 0})
            .overall == "unknown");
}

TEST_CASE("the socket frame is operation 11 with the user's own state")
{
  const auto board = std::make_shared<PresenceBoard>();
  board->apply({.userId = 3, .overall = "away", .since = 10});
  const HeartbeatService service(
      HeartbeatService::Dependencies{.board = board, .clock = [] { return int64_t{42}; }}, kPolicy);
  const Json::Value frame = service.frameFor(3);
  CHECK(frame["operation"].asInt() == 11);
  CHECK(frame["option"].asString() == "user");
  CHECK(frame["info"]["armed"].asBool());
  CHECK(frame["info"]["at"].asInt64() == 42);
  CHECK_FALSE(service.heartbeatFor(4)["armed"].asBool());
}

TEST_CASE("a presence change reaches that user's sockets at once, a repeat does not")
{
  FeedRig rig;
  CHECK(rig.feed.ingestPresence(R"({"userId":5,"overall":"away","since":9})"));
  REQUIRE(rig.emitted.size() == 1);
  CHECK(rig.emitted[0].userId == 5);
  CHECK(rig.emitted[0].frame["operation"].asInt() == 11);
  CHECK(rig.emitted[0].frame["info"]["armed"].asBool());
  CHECK_FALSE(rig.feed.ingestPresence(R"({"userId":5,"overall":"away","since":9})"));
  CHECK(rig.emitted.size() == 1);
  CHECK(rig.feed.ingestPresence(R"({"userId":5,"overall":"home","since":20})"));
  REQUIRE(rig.emitted.size() == 2);
  CHECK_FALSE(rig.emitted[1].frame["info"]["armed"].asBool());
  CHECK_FALSE(rig.feed.ingestPresence("{}"));
}

TEST_CASE("push heartbeats go to armed users only, as data-only intents")
{
  FeedRig rig;
  rig.board->apply({.userId = 1, .overall = "home", .since = 1});
  rig.board->apply({.userId = 2, .overall = "away", .since = 1});
  rig.board->apply({.userId = 3, .overall = "unknown", .since = 1});
  CHECK(rig.feed.publishPushHeartbeats() == 1);
  REQUIRE(rig.push.sent.size() == 1);
  const PushIntent& intent = rig.push.sent[0];
  CHECK(intent.userId == 2);
  CHECK(intent.type == "heartbeat");
  CHECK(intent.notificationId == 0);
  CHECK(intent.title.empty());
  CHECK(intent.body.empty());
  CHECK(intent.data["kind"].asString() == "heartbeat");
  CHECK(intent.data["armed"].asBool());
  CHECK(intent.data["at"].asInt64() == 5000);
}

TEST_CASE("a refill applies the directory and an unavailable directory keeps what is known")
{
  FeedRig rig;
  rig.directory->answer = std::vector<PresenceEntry>{
      {.userId = 1, .overall = "away", .since = 5}, {.userId = 2, .overall = "home", .since = 6}};
  CHECK(rig.feed.refill());
  CHECK(rig.board->of(1).overall == "away");
  CHECK(rig.emitted.size() == 2);
  rig.directory->answer.reset();
  CHECK_FALSE(rig.feed.refill());
  CHECK(rig.board->of(1).overall == "away");
  rig.feed.ingestGuardHeartbeat(4990);
  CHECK(rig.service->heartbeatFor(1)["guard"].asString() == "alive");
}

TEST_CASE("a refill read before a presence event never overwrites it")
{
  FeedRig rig;
  CHECK(rig.board->apply({.userId = 1, .overall = "home", .since = 10}));
  rig.directory->answer = std::vector<PresenceEntry>{
      {.userId = 1, .overall = "home", .since = 10}, {.userId = 2, .overall = "away", .since = 20}};
  rig.directory->duringList = [&rig] {
    CHECK(rig.feed.ingestPresence(R"({"userId":1,"overall":"away","since":30})"));
  };
  CHECK(rig.feed.refill());
  CHECK(rig.board->of(1).overall == "away");
  CHECK(rig.board->of(1).since == 30);
  CHECK(rig.board->of(2).overall == "away");
  CHECK(rig.board->armedUsers() == std::vector<int64_t>{1, 2});

  rig.directory->duringList = {};
  rig.directory->answer = std::vector<PresenceEntry>{{.userId = 1, .overall = "home", .since = 40}};
  rig.emitted.clear();
  CHECK(rig.feed.refill());
  CHECK(rig.board->of(1).overall == "home");
  CHECK(rig.board->of(2).overall == "unknown");
  CHECK(rig.board->armedUsers().empty());
  REQUIRE(rig.emitted.size() == 2);
  CHECK(rig.emitted[0].userId == 1);
  CHECK(rig.emitted[1].userId == 2);
}

TEST_CASE("the board fills by difference against a mark")
{
  PresenceBoard board;
  const uint64_t before = board.mark();
  CHECK(board.apply({.userId = 3, .overall = "away", .since = 5}));
  CHECK(board.mark() > before);
  const std::vector<PresenceEntry> stale{{.userId = 4, .overall = "home", .since = 1}};
  CHECK(board.fill({.entries = stale, .readMark = before}) == std::vector<int64_t>{4});
  CHECK(board.of(3).overall == "away");
  const std::vector<PresenceEntry> fresh{{.userId = 4, .overall = "home", .since = 1}};
  CHECK(board.fill({.entries = fresh, .readMark = board.mark()}) == std::vector<int64_t>{3});
  CHECK(board.of(3).overall == "unknown");
  CHECK(board.fill({.entries = fresh, .readMark = board.mark()}).empty());
}

TEST_CASE("guard's presence list maps onto the board, unknown states stay unknown")
{
  argus::guard::v1::ListPresenceResponse response;
  auto* away = response.add_users();
  away->set_user_id(4);
  away->set_overall(argus::guard::v1::PRESENCE_STATE_AWAY);
  away->set_since(70);
  auto* home = response.add_users();
  home->set_user_id(5);
  home->set_overall(argus::guard::v1::PRESENCE_STATE_HOME);
  auto* unknown = response.add_users();
  unknown->set_user_id(6);
  const auto entries = heartbeat::presenceEntriesOf(response);
  REQUIRE(entries.size() == 3);
  CHECK(entries[0].overall == "away");
  CHECK(entries[0].since == 70);
  CHECK(entries[1].overall == "home");
  CHECK(entries[2].overall == "unknown");
}
