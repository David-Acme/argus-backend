#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <auth/auth-errors.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-feed.hxx>
#include <auth/module-gate.hxx>
#include <auth/request-context.hxx>
#include <auth/role-access.hxx>
#include <auth/role-filter.hxx>
#include <doctest/doctest.h>
#include <drogon/HttpRequest.h>
#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
constexpr std::string_view kSurveillance = role_access::kSurveillanceModule;
constexpr std::string_view kProductivity = role_access::kProductivityModule;

struct FilterOutcome
{
  bool admitted{false};
  std::string code;
};

struct FilterCall
{
  UserRole role{UserRole::Guest};
  drogon::HttpMethod method{drogon::Get};
  std::string path;
};

FilterOutcome runRoleFilter(const FilterCall& call)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(call.method);
  req->setPath(call.path);
  req->getAttributes()->insert(AuthContext::kJwtKey,
                               JwtContext{.sub = 7,
                                          .name = "Ana",
                                          .role = call.role,
                                          .isActive = true,
                                          .deviceHash = "d",
                                          .sessionId = "s"});
  RoleFilter filter;
  try {
    static_cast<void>(drogon::sync_wait(filter.doFilter(req)));
    return {.admitted = true, .code = {}};
  }
  catch (const ResponseException& refusal) {
    return {.admitted = false, .code = refusal.errorCode()};
  }
}

std::string tempPath(const std::string& name)
{
  const auto path = std::filesystem::temp_directory_path() /
                    ("argus-module-gate-" + std::to_string(::getpid()) + "-" + name);
  std::filesystem::remove(path);
  return path.string();
}

bool waitUntil(const std::function<bool()>& condition)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

ModuleFeed::Config feedConfig(const std::string& stateFile)
{
  return {.stream = "TEST_SETTINGS_MODULE",
          .subject = "argus.settings.v1.module",
          .durable = "argus-test-modules",
          .stateFile = stateFile,
          .maxDeliver = 10,
          .retrySeconds = 0.01,
          .bootAttempts = 3};
}
}

TEST_CASE("every gated prefix names its module, and core routes are never gated")
{
  for (const auto* path : {"/camera", "/camera/3/webrtc", "/zone/1", "/media", "/guard/mode",
                           "/guard/panic", "/visitor", "/visitor/4/crop-preview",
                           "/visitor-settings", "/visitor-crop/abc/content"}) {
    CAPTURE(path);
    CHECK(role_access::moduleOfPath(path) == kSurveillance);
  }
  for (const auto* path : {"/project", "/project/2", "/project-task/9", "/project-member",
                           "/calendar-event/1", "/calendar-event-share/5"}) {
    CAPTURE(path);
    CHECK(role_access::moduleOfPath(path) == kProductivity);
  }
  CHECK(role_access::moduleOfPath("/CAMERA/2/") == kSurveillance);
  CHECK(role_access::moduleOfPath("/Project-Task") == kProductivity);
  for (const auto* path : {"/auth/sessions", "/user", "/person/3", "/invitation",
                           "/portrait-preview/2", "/privacy/me", "/notification",
                           "/notification-token", "/reminder", "/reminder-detail/2",
                           "/settings", "/modules", "/modules/surveillance/install",
                           "/sync/heartbeat", "/rtc/token", "/pairing", "/voiceprint/users",
                           "/llm/chat", "/tts/synthesize", "/stt", "/vlm", "/health",
                           "/camera-stream", "/cameras", "/projects", "/visitors", "/", ""}) {
    CAPTURE(path);
    CHECK_FALSE(role_access::moduleOfPath(path).has_value());
  }
}

TEST_CASE("the module routes: every role reads /modules, only the Owner changes one")
{
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(static_cast<int>(role));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/modules", .method = drogon::Get}));
    const bool owner = role == UserRole::Owner;
    for (const auto* action : {"install", "pause", "resume", "cancel", "disable", "release"}) {
      const std::string path = std::string("/modules/surveillance/") + action;
      CHECK(role_access::hasHttpAccess({.role = role, .path = path, .method = drogon::Post}) == owner);
    }
  }
}

TEST_CASE("a disabled module refuses its routes to every role with MODULE_DISABLED")
{
  moduleGate().reset();
  moduleGate().apply({{.id = std::string(kSurveillance), .enabled = false}});

  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(static_cast<int>(role));
    for (const auto* path : {"/camera", "/zone/2", "/guard/environments", "/visitor",
                             "/visitor-crop/x/content", "/camera/3/webrtc"}) {
      CAPTURE(path);
      const auto outcome = runRoleFilter({.role = role, .method = drogon::Get, .path = path});
      CHECK_FALSE(outcome.admitted);
      CHECK(outcome.code == "MODULE_DISABLED");
    }
  }

  CHECK(runRoleFilter({.role = UserRole::Owner, .method = drogon::Post, .path = "/project"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Resident, .method = drogon::Post, .path = "/project"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Guest, .method = drogon::Post, .path = "/project"}).code ==
        "FORBIDDEN");

  moduleGate().reset();
}

TEST_CASE("core routes are never gated, whatever the enabled set says")
{
  moduleGate().reset();
  moduleGate().apply({{.id = std::string(kSurveillance), .enabled = false},
                      {.id = std::string(kProductivity), .enabled = false},
                      {.id = "core", .enabled = false}});

  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(static_cast<int>(role));
    for (const auto* path : {"/notification", "/auth/sessions", "/privacy/me", "/modules",
                             "/sync/heartbeat"}) {
      CAPTURE(path);
      CHECK(runRoleFilter({.role = role, .method = drogon::Get, .path = path}).admitted);
    }
  }
  CHECK(runRoleFilter({.role = UserRole::Owner, .method = drogon::Get, .path = "/settings"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Owner, .method = drogon::Post, .path = "/reminder"}).admitted);

  moduleGate().reset();
}

TEST_CASE("an enabled module keeps the role table as it was")
{
  moduleGate().reset();
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Get, .path = "/camera"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Guest, .method = drogon::Delete, .path = "/camera/1"}).code ==
        "FORBIDDEN");
  CHECK(runRoleFilter({.role = UserRole::Guest, .method = drogon::Get, .path = "/guard/mode"}).code ==
        "FORBIDDEN");
}

TEST_CASE("the cache counts an unknown module as enabled and tells only the flips")
{
  ModuleGate gate;
  CHECK(gate.enabled(kSurveillance));
  CHECK(gate.enabled("agronomy"));
  CHECK_FALSE(gate.disabledModuleOf("/camera").has_value());

  std::vector<std::string> told;
  gate.onChange([&told](const ModuleChange& change) {
    told.push_back(change.id + (change.enabled ? ":on" : ":off"));
  });
  CHECK(gate.apply({{.id = "surveillance", .enabled = true}}).empty());
  CHECK(told.empty());

  const auto changes = gate.apply({{.id = "surveillance", .enabled = false},
                                   {.id = "productivity", .enabled = true}});
  REQUIRE(changes.size() == 1);
  CHECK(changes.front().id == "surveillance");
  CHECK(told == std::vector<std::string>{"surveillance:off"});
  CHECK(gate.disabledModuleOf("/zone") == std::optional<std::string>("surveillance"));
  CHECK_FALSE(gate.disabledModuleOf("/project").has_value());

  gate.apply({{.id = "surveillance", .enabled = true}});
  CHECK(told == std::vector<std::string>{"surveillance:off", "surveillance:on"});
  CHECK(gate.enabled(kSurveillance));
}

TEST_CASE("the enabled set parses strictly")
{
  const auto parsed = module_gate::parseEnabledSet(
      R"({"modules":[{"id":"surveillance","enabled":false},{"id":"productivity","enabled":true}],"version":4})");
  REQUIRE(parsed.has_value());
  const ModuleFlags flags = parsed.value_or(ModuleFlags{});
  REQUIRE(flags.size() == 2);
  CHECK(flags[0].id == "surveillance");
  CHECK_FALSE(flags[0].enabled);
  CHECK(flags[1].enabled);

  const auto lifecycles = module_gate::parseEnabledSet(
      R"({"modules":[{"id":"a","enabled":true,"lifecycle":"active"},
                     {"id":"b","enabled":true,"lifecycle":"disabled"},
                     {"id":"c","enabled":true,"lifecycle":"not_installed"},
                     {"id":"d","enabled":true,"lifecycle":"uninstalled_data_kept"},
                     {"id":"e","enabled":false,"lifecycle":"active"}]})");
  const ModuleFlags states = lifecycles.value_or(ModuleFlags{});
  REQUIRE(states.size() == 5);
  CHECK(states[0].enabled);
  for (std::size_t i = 1; i < states.size(); ++i) {
    CAPTURE(states[i].id);
    CHECK_FALSE(states[i].enabled);
  }
  CHECK_FALSE(module_gate::parseEnabledSet(R"({"modules":[{"id":"a","enabled":true,"lifecycle":1}]})").has_value());

  CHECK_FALSE(module_gate::parseEnabledSet("not json").has_value());
  CHECK_FALSE(module_gate::parseEnabledSet(R"({"modules":{}})").has_value());
  CHECK_FALSE(module_gate::parseEnabledSet(R"({"modules":[{"id":"Surveillance","enabled":true}]})").has_value());
  CHECK_FALSE(module_gate::parseEnabledSet(R"({"modules":[{"id":"surveillance","enabled":"no"}]})").has_value());
  CHECK_FALSE(module_gate::parseEnabledSet(R"({"modules":[{"id":"","enabled":true}]})").has_value());

  const auto again = module_gate::parseEnabledSet(module_gate::serializeEnabledSet(flags));
  REQUIRE(again.has_value());
  CHECK(again.value_or(ModuleFlags{}).size() == 2);
}

TEST_CASE("a fresh install with no state, no file and no settings gates nothing")
{
  ModuleGate gate;
  const std::string file = tempPath("fresh.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
  feed.restore();
  CHECK(gate.enabled(kSurveillance));
  CHECK(gate.enabled(kProductivity));
  CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("a live update applies, persists, and survives a restart without settings")
{
  ModuleGate gate;
  const std::string file = tempPath("live.json");
  {
    ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
    CHECK(feed.handle(R"({"modules":[{"id":"surveillance","enabled":false}],"version":3,"settled":true})") ==
          ModuleFeedDisposition::Applied);
    CHECK_FALSE(gate.enabled(kSurveillance));
    CHECK(feed.version() == 3);

    CHECK(feed.handle(R"({"modules":[{"id":"surveillance","enabled":true}],"version":2})") ==
          ModuleFeedDisposition::Ignored);
    CHECK_FALSE(gate.enabled(kSurveillance));

    CHECK(feed.handle(R"({"modules":[{"id":"surveillance","enabled":true}],"settled":false,"version":9})") ==
          ModuleFeedDisposition::Ignored);
    CHECK_FALSE(gate.enabled(kSurveillance));

    CHECK(feed.handle(R"({"modules":[{"id":"surveillance","enabled":true,"lifecycle":"uninstalled_data_kept"}],"version":4})") ==
          ModuleFeedDisposition::Applied);
    CHECK_FALSE(gate.enabled(kSurveillance));

    CHECK(feed.handle(R"({"module":{"id":"surveillance","job":{"progress":0.5}}})") ==
          ModuleFeedDisposition::Ignored);
    CHECK(feed.handle(R"({"modules":[{"id":7}]})") == ModuleFeedDisposition::Refused);
    CHECK(feed.handle("[]") == ModuleFeedDisposition::Refused);
    CHECK_FALSE(gate.enabled(kSurveillance));
  }
  REQUIRE(std::filesystem::exists(file));

  ModuleGate restarted;
  ModuleFeed again({.bus = nullptr, .gate = &restarted, .bootRead = {}}, feedConfig(file));
  CHECK(restarted.enabled(kSurveillance));
  again.restore();
  CHECK_FALSE(restarted.enabled(kSurveillance));
  CHECK(restarted.enabled(kProductivity));
  std::filesystem::remove(file);
}

namespace
{
std::string enabledEvent(bool surveillance, int version, const std::string& epoch)
{
  return std::string(R"({"modules":[{"id":"surveillance","enabled":)") + (surveillance ? "true" : "false") +
         R"(}],"settled":true,"version":)" + std::to_string(version) +
         (epoch.empty() ? std::string() : R"(,"epoch":")" + epoch + "\"") + "}";
}
}

TEST_CASE("a feed whose durable the previous build made under another delivery policy works with no restart" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  NatsBus::Options options;
  options.url = broker;
  auto bus = std::make_shared<NatsBus>();
  REQUIRE(bus->connect(options));

  const std::string stream = "argus-test-module-feed-" + std::to_string(::getpid());
  const std::string subject = stream + ".module";
  REQUIRE(bus->ensureStream({.name = stream,
                             .subjects = {subject},
                             .maxAgeNs = 60LL * 1000000000,
                             .duplicatesNs = 60LL * 1000000000}));
  const auto publish = [&](const std::string& body, const std::string& id) {
    return bus->publishWithMsgId({.subject = subject, .payload = body, .msgId = subject + "-" + id});
  };
  REQUIRE(publish(enabledEvent(true, 40, "1000-old"), "old"));

  const std::string durable = "argus-test-feed-modules";
  const auto previousBuild = bus->subscribeDurable({.stream = stream,
                                                    .durable = durable,
                                                    .subject = subject,
                                                    .deliverAll = true,
                                                    .maxDeliver = 10,
                                                    .maxAckPending = NatsBus::kOrderedMaxAckPending,
                                                    .handler = [](const NatsBus::DurableMessage&,
                                                                  const NatsBus::DurableSettlement& settlement) {
                                                      settlement.ack();
                                                    }});
  REQUIRE(previousBuild.has_value());
  REQUIRE(bus->unsubscribe(previousBuild.value_or(0)));

  ModuleGate gate;
  const std::string file = tempPath("feed-upgrade.json");
  auto config = feedConfig(file);
  config.stream = stream;
  config.subject = subject;
  config.durable = durable;
  config.retrySeconds = 0.05;
  ModuleFeed feed({.bus = bus, .gate = &gate, .bootRead = {}}, config);
  feed.start();
  REQUIRE(waitUntil([&feed] { return feed.drained(); }));

  REQUIRE(publish(enabledEvent(false, 1, "2000-new"), "disable"));
  CHECK(waitUntil([&gate] { return !gate.enabled(kSurveillance); }));
  REQUIRE(publish(enabledEvent(true, 2, "2000-new"), "enable"));
  CHECK(waitUntil([&gate] { return gate.enabled(kSurveillance); }));
  feed.requestStop();
  bus->drain();
  std::filesystem::remove(file);
}

TEST_CASE("a lower version under a new epoch applies, under the same epoch it is ignored")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-lower.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));

  CHECK(feed.handle(enabledEvent(false, 40, "1000-aa")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.version() == 40);
  CHECK(feed.epoch() == "1000-aa");

  CHECK(feed.handle(enabledEvent(true, 12, "1000-aa")) == ModuleFeedDisposition::Ignored);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.version() == 40);

  CHECK(feed.handle(enabledEvent(true, 3, "2000-bb")) == ModuleFeedDisposition::Applied);
  CHECK(gate.enabled(kSurveillance));
  CHECK(feed.version() == 3);
  CHECK(feed.epoch() == "2000-bb");

  CHECK(feed.handle(enabledEvent(false, 2, "2000-bb")) == ModuleFeedDisposition::Ignored);
  CHECK(gate.enabled(kSurveillance));
  CHECK(feed.handle(enabledEvent(false, 4, "2000-bb")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  std::filesystem::remove(file);
}

TEST_CASE("a different epoch is always adopted, whatever its text or its mint time looks like")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-opaque.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
  CHECK(feed.handle(enabledEvent(false, 90, "zulu")) == ModuleFeedDisposition::Applied);
  CHECK(feed.handle(enabledEvent(true, 1, "alfa")) == ModuleFeedDisposition::Applied);
  CHECK(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "alfa");
  CHECK(feed.version() == 1);
  std::filesystem::remove(file);
}

TEST_CASE("a host whose clock went backwards across a restart still adopts the new epoch")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-clock.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
  CHECK(feed.handle(enabledEvent(false, 80, "1791300000000-aaaaaaaaaaaaaaaa")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));

  CHECK(feed.handle(enabledEvent(true, 2, "946684800000-bbbbbbbbbbbbbbbb")) == ModuleFeedDisposition::Applied);
  CHECK(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "946684800000-bbbbbbbbbbbbbbbb");
  CHECK(feed.version() == 2);

  CHECK(feed.handle(enabledEvent(false, 3, "946684800000-bbbbbbbbbbbbbbbb")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  std::filesystem::remove(file);
}

TEST_CASE("a stray message of an old epoch after adoption is corrected by the next ModuleStates re-pull")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-stray.json");
  std::atomic<int> pulls{0};
  ModuleFeed feed({.bus = nullptr,
                   .gate = &gate,
                   .bootRead = [&pulls]() -> std::optional<ModuleFeed::Snapshot> {
                     ++pulls;
                     return ModuleFeed::Snapshot{
                         .flags = {{.id = std::string(kSurveillance), .enabled = false, .lifecycle = "disabled"}},
                         .version = 5,
                         .epoch = "5000-new"};
                   }},
                  feedConfig(file));
  CHECK(feed.handle(enabledEvent(false, 5, "5000-new")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  const int adopted = pulls.load();

  CHECK(feed.handle(enabledEvent(true, 900, "1000-old")) == ModuleFeedDisposition::Applied);
  CHECK(pulls.load() == adopted + 1);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "5000-new");
  CHECK(feed.version() == 5);

  CHECK(feed.handle(enabledEvent(true, 6, "5000-new")) == ModuleFeedDisposition::Applied);
  CHECK(pulls.load() == adopted + 1);
  CHECK(gate.enabled(kSurveillance));
  std::filesystem::remove(file);
}

TEST_CASE("when settings does not answer the re-pull the adopted message stands")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-silent.json");
  std::atomic<int> pulls{0};
  ModuleFeed feed({.bus = nullptr,
                   .gate = &gate,
                   .bootRead = [&pulls]() -> std::optional<ModuleFeed::Snapshot> {
                     ++pulls;
                     return std::nullopt;
                   }},
                  feedConfig(file));
  CHECK(feed.handle(enabledEvent(false, 3, "2000-aa")) == ModuleFeedDisposition::Applied);
  CHECK(pulls.load() == 1);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "2000-aa");
  std::filesystem::remove(file);
}

TEST_CASE("a feed without an epoch keeps judging by version alone")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-none.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
  CHECK(feed.handle(enabledEvent(false, 5, "")) == ModuleFeedDisposition::Applied);
  CHECK(feed.handle(enabledEvent(true, 4, "")) == ModuleFeedDisposition::Ignored);
  CHECK(feed.handle(enabledEvent(true, 6, "")) == ModuleFeedDisposition::Applied);
  CHECK(feed.epoch().empty());
  CHECK(feed.handle(enabledEvent(false, 7, "3000-x")) == ModuleFeedDisposition::Applied);
  CHECK(feed.handle(enabledEvent(true, 1, "")) == ModuleFeedDisposition::Ignored);
  std::filesystem::remove(file);
}

TEST_CASE("a fresh consumer over a stream holding an old epoch's high versions and a new epoch's low ones ends on the new state")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-replay.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));

  for (int version = 60; version <= 64; ++version)
    CHECK(feed.handle(enabledEvent(version % 2 == 0, version, "1000-old")) == ModuleFeedDisposition::Applied);
  CHECK(feed.version() == 64);

  CHECK(feed.handle(enabledEvent(false, 1, "9000-reset")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.handle(enabledEvent(true, 2, "9000-reset")) == ModuleFeedDisposition::Applied);
  CHECK(gate.enabled(kSurveillance));
  CHECK(feed.handle(enabledEvent(false, 3, "9000-reset")) == ModuleFeedDisposition::Applied);
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "9000-reset");
  CHECK(feed.version() == 3);
  std::filesystem::remove(file);
}

TEST_CASE("the boot read and the feed share one epoch: a snapshot of a new epoch resets the version it judges by")
{
  ModuleGate gate;
  const std::string file = tempPath("epoch-boot.json");
  ModuleFeed feed({.bus = nullptr, .gate = &gate, .bootRead = {}}, feedConfig(file));
  CHECK(feed.handle(enabledEvent(true, 80, "1000-old")) == ModuleFeedDisposition::Applied);
  CHECK(feed.applyAuthoritative({.flags = {{.id = std::string(kSurveillance), .enabled = false, .lifecycle = "disabled"}},
                                 .version = 2,
                                 .epoch = "7000-new"}));
  CHECK_FALSE(gate.enabled(kSurveillance));
  CHECK(feed.epoch() == "7000-new");
  CHECK(feed.version() == 2);
  CHECK_FALSE(feed.applyAuthoritative({.flags = {{.id = std::string(kSurveillance), .enabled = true, .lifecycle = "active"}},
                                       .version = 1,
                                       .epoch = "7000-new"}));
  CHECK_FALSE(gate.enabled(kSurveillance));
  std::filesystem::remove(file);
}

TEST_CASE("the boot read wins over the last known state, and retries until settings answers")
{
  ModuleGate gate;
  const std::string file = tempPath("boot.json");
  REQUIRE(module_gate::saveStateFile(file, {{.id = "surveillance", .enabled = false}}));

  std::atomic<int> calls{0};
  ModuleFeed feed({.bus = nullptr,
                   .gate = &gate,
                   .bootRead = [&calls]() -> std::optional<ModuleFeed::Snapshot> {
                     if (calls.fetch_add(1) == 0)
                       return std::nullopt;
                     return ModuleFeed::Snapshot{
                         .flags = {{.id = "surveillance", .enabled = true},
                                   {.id = "productivity", .enabled = false}},
                         .version = 5};
                   }},
                  feedConfig(file));
  feed.restore();
  CHECK_FALSE(gate.enabled(kSurveillance));
  feed.start();
  CHECK(waitUntil([&gate] { return gate.enabled(kSurveillance) && !gate.enabled(kProductivity); }));
  CHECK(waitUntil([&feed] { return feed.drained(); }));
  CHECK(calls.load() == 2);
  CHECK(feed.version() == 5);

  const auto persisted = module_gate::loadStateFile(file);
  REQUIRE(persisted.has_value());
  CHECK(persisted.value_or(ModuleFlags{}).size() == 2);
  std::filesystem::remove(file);
}

TEST_CASE("a boot read that never answers leaves the last known state in force")
{
  ModuleGate gate;
  const std::string file = tempPath("silent.json");
  REQUIRE(module_gate::saveStateFile(file, {{.id = "productivity", .enabled = false}}));
  std::atomic<int> calls{0};
  ModuleFeed feed({.bus = nullptr,
                   .gate = &gate,
                   .bootRead = [&calls]() -> std::optional<ModuleFeed::Snapshot> {
                     calls.fetch_add(1);
                     return std::nullopt;
                   }},
                  feedConfig(file));
  feed.restore();
  feed.start();
  CHECK(waitUntil([&feed, &calls] { return feed.drained() && calls.load() == 3; }));
  CHECK_FALSE(gate.enabled(kProductivity));
  CHECK(gate.enabled(kSurveillance));
  std::filesystem::remove(file);
}

TEST_CASE("the enabled set carries the roles, texts and lifecycle of each module and reads them back")
{
  const auto parsed = module_gate::parseEnabledSet(
      R"({"modules":[{"id":"surveillance","enabled":true,"lifecycle":"active","dataPurgedAt":1700,
          "kind":"available","roles":["guard"],"name":{"es":"Vigilancia","en":"Surveillance"},
          "summary":{"es":"Cámaras","en":"Cameras"},
          "intro":{"es":{"what":"Qué es","examples":["a","b","c"]},"en":{"what":"What","examples":["x"]}}},
         {"id":"core","enabled":true}],"version":2})");
  REQUIRE(parsed.has_value());
  const ModuleFlags flags = parsed.value_or(ModuleFlags{});
  REQUIRE(flags.size() == 2);
  CHECK(flags[0].roles == std::vector<std::string>{"guard"});
  CHECK(flags[0].name.es == "Vigilancia");
  CHECK(flags[0].name.en == "Surveillance");
  CHECK(flags[0].summary.en == "Cameras");
  CHECK(flags[0].intro.es.what == "Qué es");
  CHECK(flags[0].intro.es.examples == std::vector<std::string>{"a", "b", "c"});
  CHECK(flags[0].intro.en.examples.size() == 1);
  CHECK(flags[0].dataPurgedAt == 1700);
  CHECK(flags[0].lifecycle == "active");
  CHECK(flags[0].kind == "available");
  CHECK(flags[1].kind.empty());
  CHECK(flags[1].roles.empty());

  const auto again = module_gate::parseEnabledSet(module_gate::serializeEnabledSet(flags));
  REQUIRE(again.has_value());
  CHECK(again.value_or(ModuleFlags{}) == flags);

  for (const auto* body : {
           R"({"modules":[{"id":"a","enabled":true,"roles":"guard"}]})",
           R"({"modules":[{"id":"a","enabled":true,"roles":["Guard"]}]})",
           R"({"modules":[{"id":"a","enabled":true,"roles":[1]}]})",
           R"({"modules":[{"id":"a","enabled":true,"name":"x"}]})",
           R"({"modules":[{"id":"a","enabled":true,"name":{"es":1}}]})",
           R"({"modules":[{"id":"a","enabled":true,"intro":{"es":{"what":7}}}]})",
           R"({"modules":[{"id":"a","enabled":true,"intro":{"es":{"what":"x","examples":"y"}}}]})",
           R"({"modules":[{"id":"a","enabled":true,"intro":{"es":{"what":"x","examples":[1]}}}]})",
           R"({"modules":[{"id":"a","enabled":true,"dataPurgedAt":"soon"}]})",
           R"({"modules":[{"id":"a","enabled":true,"kind":4}]})"}) {
    CAPTURE(body);
    CHECK_FALSE(module_gate::parseEnabledSet(body).has_value());
  }
}

TEST_CASE("a role is inactive while its module is not, and every other role never is")
{
  ModuleGate gate;
  CHECK(gate.roleActive(UserRole::Guard));
  gate.apply({{.id = "surveillance", .enabled = false, .lifecycle = "disabled", .dataPurgedAt = 0,
               .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}},
              {.id = "productivity", .enabled = true, .lifecycle = "active", .dataPurgedAt = 0,
               .roles = {}, .name = {}, .summary = {}, .intro = {}}});
  CHECK_FALSE(gate.roleActive(UserRole::Guard));
  CHECK(gate.roleActive(UserRole::Owner));
  CHECK(gate.roleActive(UserRole::Resident));
  CHECK(gate.roleActive(UserRole::Guest));
  CHECK_FALSE(gate.roleActive(UserRole::Unknown));
  CHECK(gate.snapshot().moduleOfRole(UserRole::Guard) == std::optional<std::string_view>("surveillance"));
  CHECK_FALSE(gate.snapshot().moduleOfRole(UserRole::Resident).has_value());

  gate.apply({{.id = "surveillance", .enabled = true, .lifecycle = "active", .dataPurgedAt = 0,
               .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});
  CHECK(gate.roleActive(UserRole::Guard));
}

TEST_CASE("an owner can never be made inactive by a catalog that names it")
{
  ModuleGate gate;
  gate.apply({{.id = "surveillance", .enabled = false, .lifecycle = "disabled", .dataPurgedAt = 0,
               .roles = {"owner"}, .name = {}, .summary = {}, .intro = {}}});
  CHECK(gate.roleActive(UserRole::Owner));
}

TEST_CASE("the state listener fires once per change of any field and not for a repeat")
{
  ModuleGate gate;
  int told = 0;
  gate.onStateChange([&told] { ++told; });
  const ModuleFlag base{.id = "surveillance", .enabled = true, .lifecycle = "active", .dataPurgedAt = 0,
                        .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}};
  gate.apply({base});
  CHECK(told == 1);
  gate.apply({base});
  CHECK(told == 1);
  ModuleFlag renamed = base;
  renamed.name.es = "Vigilancia";
  gate.apply({renamed});
  CHECK(told == 2);
  ModuleFlag purged = renamed;
  purged.dataPurgedAt = 99;
  gate.apply({purged});
  CHECK(told == 3);
  ModuleFlag withoutRole = purged;
  withoutRole.roles.clear();
  gate.apply({withoutRole});
  CHECK(told == 4);
}

TEST_CASE("the roles and texts survive the last known state file")
{
  const std::string file = tempPath("roles.json");
  ModuleGate first;
  ModuleFeed feed({.bus = nullptr, .gate = &first, .bootRead = {}}, feedConfig(file));
  CHECK(feed.handle(
            R"({"modules":[{"id":"surveillance","enabled":false,"lifecycle":"disabled","roles":["guard"],
                "name":{"es":"Vigilancia","en":"Surveillance"},
                "intro":{"es":{"what":"Qué","examples":["uno"]},"en":{"what":"What","examples":["one"]}}}],"version":2})") ==
        ModuleFeedDisposition::Applied);

  ModuleGate second;
  ModuleFeed restarted({.bus = nullptr, .gate = &second, .bootRead = {}}, feedConfig(file));
  restarted.restore();
  CHECK_FALSE(second.roleActive(UserRole::Guard));
  const auto known = second.known();
  REQUIRE(known.size() == 1);
  CHECK(known[0].name.en == "Surveillance");
  CHECK(known[0].intro.en.examples == std::vector<std::string>{"one"});
  std::filesystem::remove(file);
}

TEST_CASE("RoleFilter keeps panic open while surveillance is off and names an inactive role")
{
  moduleGate().reset();
  moduleGate().apply({{.id = std::string(kSurveillance), .enabled = false, .lifecycle = "disabled",
                       .dataPurgedAt = 0, .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});

  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CAPTURE(static_cast<int>(role));
    CHECK(runRoleFilter({.role = role, .method = drogon::Post, .path = "/guard/panic"}).admitted);
    CHECK(runRoleFilter({.role = role, .method = drogon::Get, .path = "/guard/safety"}).admitted);
    CHECK(runRoleFilter({.role = role, .method = drogon::Post, .path = "/guard/mode"}).code ==
          "MODULE_DISABLED");
  }

  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Get, .path = "/person"}).code ==
        "ROLE_INACTIVE");
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Get, .path = "/auth/sessions"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Patch, .path = "/reminder/2"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Resident, .method = drogon::Get, .path = "/person"}).admitted);
  CHECK(runRoleFilter({.role = UserRole::Guest, .method = drogon::Get, .path = "/memory"}).code ==
        "FORBIDDEN");

  moduleGate().apply({{.id = std::string(kSurveillance), .enabled = true, .lifecycle = "active",
                       .dataPurgedAt = 0, .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Get, .path = "/person"}).admitted);
  moduleGate().reset();
}

TEST_CASE("RoleFilter refuses a role this build does not know everywhere, even on a core route")
{
  moduleGate().reset();
  for (const auto* path : {"/person", "/reminder", "/auth/sessions", "/privacy/me", "/modules",
                           "/guard/panic", "/camera", "/user"}) {
    CAPTURE(path);
    const auto outcome = runRoleFilter({.role = UserRole::Unknown,
                                        .method = path == std::string("/guard/panic") ? drogon::Post : drogon::Get,
                                        .path = path});
    CHECK_FALSE(outcome.admitted);
    CHECK(outcome.code == "FORBIDDEN");
  }
}

TEST_CASE("RoleFilter lets an inactive guard read and answer a raised alert while surveillance is off")
{
  moduleGate().reset();
  moduleGate().apply({{.id = std::string(kSurveillance), .enabled = false, .lifecycle = "disabled",
                       .dataPurgedAt = 0, .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});
  struct Request
  {
    drogon::HttpMethod method;
    const char* path;
  };
  for (const auto& request : {Request{.method = drogon::Get, .path = "/guard/safety"},
                              Request{.method = drogon::Get, .path = "/notification/responses"},
                              Request{.method = drogon::Get, .path = "/notification/responses/4"},
                              Request{.method = drogon::Patch, .path = "/notification/responses/4"},
                              Request{.method = drogon::Post, .path = "/rtc/token"},
                              Request{.method = drogon::Post, .path = "/guard/panic"}}) {
    CAPTURE(request.path);
    for (const auto role : {UserRole::Guard, UserRole::Resident, UserRole::Guest, UserRole::Owner})
      CHECK(runRoleFilter({.role = role, .method = request.method, .path = request.path}).admitted);
  }
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Put, .path = "/guard/safety/pin"}).code ==
        "MODULE_DISABLED");
  CHECK(runRoleFilter({.role = UserRole::Owner, .method = drogon::Patch, .path = "/guard/safety"}).code ==
        "MODULE_DISABLED");
  CHECK(runRoleFilter({.role = UserRole::Guard, .method = drogon::Post, .path = "/guard/environments/1/duty"}).code ==
        "MODULE_DISABLED");
  moduleGate().reset();
}
