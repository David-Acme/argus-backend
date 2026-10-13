#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <json/json.h>
#include <map>
#include <memory>
#include <shared/services/tapo/tapo-api.hxx>
#include <shared/services/tapo/tapo-client.hxx>
#include <shared/services/tapo/tapo-control-hub.hxx>
#include <shared/services/tapo/tapo-lockout.hxx>
#include <shared/services/tapo/tapo-recordings.hxx>
#include <string>
#include <utility>
#include <vector>
#include <optional>
#include <stdexcept>

template <typename T>
const T& requireValue(const std::optional<T>& value)
{
  if (!value)
    throw std::runtime_error("test: expected a value");
  return *value;
}

namespace
{
enum class Answer : uint8_t
{
  Accept,
  Refuse,
  Lock,
  Offline
};

struct Rule
{
  std::string username;
  Answer answer{Answer::Refuse};
  int secLeft{0};
};

struct Ledger
{
  std::vector<std::pair<std::string, TapoTransportKind>> logins;
  std::vector<Rule> rules;
  int64_t nowMs{1'000'000};
};

class FakeTransport final : public ITapoTransport
{
public:
  FakeTransport(std::shared_ptr<Ledger> ledger, TapoTransportRequest request)
      : ledger_(std::move(ledger)), request_(std::move(request))
  {
  }

  TapoResult login() override
  {
    ledger_->logins.emplace_back(request_.credentials.username, request_.kind);
    for (const auto& rule : ledger_->rules) {
      if (rule.username != request_.credentials.username)
        continue;
      switch (rule.answer) {
        case Answer::Accept:
          authenticated_ = true;
          return TapoResult::success(Json::Value());
        case Answer::Refuse:
          return TapoResult::failure("login rejected", -40401)
              .as(TapoFailureKind::CredentialRefused);
        case Answer::Lock:
          return TapoResult::failure("camera locked login", tapo_lockout::kLockedOutCode)
              .as(TapoFailureKind::LockedOut, rule.secLeft);
        case Answer::Offline:
          return TapoResult::failure("transport error").as(TapoFailureKind::Transport);
      }
    }
    return TapoResult::failure("login rejected", -40401).as(TapoFailureKind::CredentialRefused);
  }

  TapoResult request(const Json::Value&) override { return TapoResult::success(Json::Value()); }
  [[nodiscard]] TapoTransportKind kind() const override { return request_.kind; }
  [[nodiscard]] Json::Value state() const override { return {Json::objectValue}; }
  [[nodiscard]] bool isAuthenticated() const override { return authenticated_; }

private:
  std::shared_ptr<Ledger> ledger_;
  TapoTransportRequest request_;
  bool authenticated_{false};
};

struct Bench
{
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::vector<TapoWinnerMemory> persisted;

  TapoClientConfig config(const std::string& host = "10.0.0.5")
  {
    TapoClientConfig config;
    config.host = host;
    config.port = 443;
    config.transport = TapoTransportPreference::SecurePassthrough;
    config.loginAttempts = 2;
    config.candidates = {
        {.label = "cloud_admin", .username = "admin", .password = "cloud", .memoryKey = "k-cloud"},
        {.label = "camera_account", .username = "camera-user", .password = "cam", .memoryKey = "k-cam"}};
    config.transportFactory = [ledger = ledger](const TapoTransportRequest& request) {
      return std::make_unique<FakeTransport>(ledger, request);
    };
    config.nowMs = [ledger = ledger]() { return ledger->nowMs; };
    config.persistWinner = [this](const TapoWinnerMemory& winner) { persisted.push_back(winner); };
    return config;
  }

  void advance(int seconds) { ledger->nowMs += static_cast<int64_t>(seconds) * 1000; }
  [[nodiscard]] std::size_t logins() const { return ledger->logins.size(); }
};
}

TEST_CASE("the cloud account is tried first and remembered once it wins")
{
  tapo_control_hub::reset();
  Bench bench;
  bench.ledger->rules = {{.username = "admin", .answer = Answer::Accept, .secLeft = 0}};
  TapoClient client(bench.config());

  const auto connected = client.connect();
  REQUIRE(connected.ok);
  CHECK(bench.logins() == 1);
  CHECK(bench.ledger->logins.front().first == "admin");
  CHECK(client.credentialLabel() == "cloud_admin");
  REQUIRE(bench.persisted.size() == 1);
  CHECK(bench.persisted.front().label == "cloud_admin");
  CHECK(bench.persisted.front().key == "k-cloud");
  CHECK(client.status().state == TapoControlState::Ready);
}

TEST_CASE("a remembered winner is the only credential tried")
{
  tapo_control_hub::reset();
  Bench bench;
  bench.ledger->rules = {{.username = "camera-user", .answer = Answer::Accept, .secLeft = 0}};
  auto config = bench.config();
  config.remembered = {.label = "camera_account", .key = "k-cam"};
  TapoClient client(std::move(config));

  REQUIRE(client.connect().ok);
  CHECK(bench.logins() == 1);
  CHECK(bench.ledger->logins.front().first == "camera-user");
  CHECK(bench.persisted.empty());
}

TEST_CASE("a lockout stops the cycle at once and blocks every client of that camera")
{
  tapo_control_hub::reset();
  Bench bench;
  bench.ledger->rules = {{.username = "admin", .answer = Answer::Lock, .secLeft = 68}};
  TapoClient client(bench.config());

  const auto locked = client.connect();
  CHECK_FALSE(locked.ok);
  CHECK(locked.kind == TapoFailureKind::LockedOut);
  CHECK(bench.logins() == 1);
  CHECK(client.status().state == TapoControlState::LockedOut);
  CHECK(tapo_control::retryAfterSeconds(client.status(), bench.ledger->nowMs) == 70);

  bench.advance(10);
  CHECK_FALSE(client.connect().ok);
  CHECK(bench.logins() == 1);

  TapoClient sibling(bench.config());
  CHECK_FALSE(sibling.connect().ok);
  CHECK(sibling.status().state == TapoControlState::LockedOut);
  CHECK(bench.logins() == 1);

  bench.ledger->rules = {{.username = "admin", .answer = Answer::Accept, .secLeft = 0}};
  bench.advance(61);
  REQUIRE(client.connect().ok);
  CHECK(bench.logins() == 2);
  CHECK(client.status().state == TapoControlState::Ready);
}

TEST_CASE("refused credentials cost one exchange each and are retried once every ten minutes")
{
  tapo_control_hub::reset();
  Bench bench;
  TapoClient client(bench.config());

  const auto refused = client.connect();
  CHECK_FALSE(refused.ok);
  CHECK(bench.logins() == 2);
  CHECK(client.status().state == TapoControlState::Refused);

  bench.advance(300);
  CHECK_FALSE(client.connect().ok);
  CHECK(bench.logins() == 2);

  bench.advance(301);
  bench.ledger->rules = {{.username = "admin", .answer = Answer::Accept, .secLeft = 0}};
  REQUIRE(client.connect().ok);
  CHECK(bench.logins() == 3);
  CHECK(client.status().state == TapoControlState::Ready);
}

TEST_CASE("an unreachable control port backs off from five seconds and doubles")
{
  tapo_control_hub::reset();
  Bench bench;
  bench.ledger->rules = {{.username = "admin", .answer = Answer::Offline, .secLeft = 0}};
  TapoClient client(bench.config());

  CHECK_FALSE(client.connect().ok);
  CHECK(bench.logins() == 2);
  CHECK(client.status().state == TapoControlState::Unreachable);
  CHECK(tapo_control::retryAfterSeconds(client.status(), bench.ledger->nowMs) == 5);

  bench.advance(4);
  CHECK_FALSE(client.connect().ok);
  CHECK(bench.logins() == 2);

  bench.advance(2);
  CHECK_FALSE(client.connect().ok);
  CHECK(bench.logins() == 4);
  CHECK(tapo_control::retryAfterSeconds(client.status(), bench.ledger->nowMs) == 10);
}

TEST_CASE("the control status is spoken as data the app can show")
{
  tapo_control_hub::reset();
  Bench bench;
  bench.ledger->rules = {{.username = "admin", .answer = Answer::Lock, .secLeft = 30}};
  TapoClient client(bench.config());
  CHECK_FALSE(client.connect().ok);

  const Json::Value json = tapo_control::toJson(client.status(), bench.ledger->nowMs);
  CHECK(json["state"].asString() == "locked_out");
  CHECK(json["retryAfterSeconds"].asInt() == 32);
  CHECK(json["code"].asInt() == tapo_lockout::kLockedOutCode);
  CHECK(json["message"].asString().find("locked") != std::string::npos);
}

TEST_CASE("a lockout reading is parsed from the camera's own reply shapes")
{
  Json::Value nested(Json::objectValue);
  nested["error_code"] = -40401;
  nested["result"]["data"]["code"] = -40404;
  nested["result"]["data"]["sec_left"] = 1650;
  const auto reading = tapo_lockout::of(nested);
  REQUIRE(reading.has_value());
  CHECK(requireValue(reading).secLeft == 1650);
  CHECK(requireValue(reading).code == -40404);

  Json::Value flat(Json::objectValue);
  flat["data"]["code"] = -40404;
  flat["data"]["sec_left"] = "68";
  flat["error_code"] = -40401;
  const auto flatReading = tapo_lockout::of(flat);
  REQUIRE(flatReading.has_value());
  CHECK(requireValue(flatReading).secLeft == 68);

  Json::Value refused(Json::objectValue);
  refused["error_code"] = -40401;
  CHECK_FALSE(tapo_lockout::of(refused).has_value());
}

namespace
{
struct ScriptedCamera
{
  std::map<std::string, Json::Value> answers;
  std::map<std::string, int> refusals;
  std::vector<Json::Value> asked;
};

class ScriptedTransport final : public ITapoTransport
{
public:
  explicit ScriptedTransport(std::shared_ptr<ScriptedCamera> camera) : camera_(std::move(camera)) {}

  TapoResult login() override
  {
    authenticated_ = true;
    return TapoResult::success(Json::Value());
  }

  TapoResult request(const Json::Value& payload) override
  {
    const Json::Value& inner = payload["params"]["requests"][0];
    camera_->asked.push_back(inner);
    const std::string method = inner["method"].asString();
    Json::Value response(Json::objectValue);
    response["method"] = method;
    const auto refused = camera_->refusals.find(method);
    response["error_code"] = refused == camera_->refusals.end() ? 0 : refused->second;
    const auto answer = camera_->answers.find(method);
    response["result"] = answer == camera_->answers.end() ? Json::Value(Json::objectValue) : answer->second;
    Json::Value data(Json::objectValue);
    data["error_code"] = 0;
    data["result"]["responses"].append(response);
    return TapoResult::success(data);
  }

  [[nodiscard]] TapoTransportKind kind() const override { return TapoTransportKind::SecurePassthrough; }
  [[nodiscard]] Json::Value state() const override { return {Json::objectValue}; }
  [[nodiscard]] bool isAuthenticated() const override { return authenticated_; }

private:
  std::shared_ptr<ScriptedCamera> camera_;
  bool authenticated_{false};
};

TapoClientConfig scriptedConfig(const std::shared_ptr<ScriptedCamera>& camera)
{
  TapoClientConfig config;
  config.host = "10.0.0.40";
  config.port = 443;
  config.transport = TapoTransportPreference::SecurePassthrough;
  config.candidates = {{.label = "camera_account", .username = "camera-user", .password = "cam", .memoryKey = "k"}};
  config.transportFactory = [camera](const TapoTransportRequest&) {
    return std::make_unique<ScriptedTransport>(camera);
  };
  config.nowMs = [] { return int64_t{1'000'000}; };
  return config;
}

Json::Value parsed(const std::string& text)
{
  Json::Value value;
  Json::Reader reader;
  REQUIRE(reader.parse(text, value));
  return value;
}
}

TEST_CASE("an SD search date is the camera's compact form, and only a well formed one")
{
  CHECK(tapo_recordings::deviceDate("2026-10-13") == std::optional<std::string>("20261013"));
  CHECK_FALSE(tapo_recordings::deviceDate("2026-1-13").has_value());
  CHECK_FALSE(tapo_recordings::deviceDate("2026/10/13").has_value());
  CHECK_FALSE(tapo_recordings::deviceDate("20261013").has_value());
  CHECK_FALSE(tapo_recordings::deviceDate("2026-1x-13").has_value());
}

TEST_CASE("the camera's day list reads in either shape and keeps only real days, sorted once")
{
  const Json::Value results = parsed(R"([
    {"search_results_1": {"date": "20261012"}},
    {"date": "20261001"},
    {"search_results_3": {"date": "20261012"}},
    {"search_results_4": {"date": "2026101"}},
    {"search_results_5": {"date": 20261003}},
    {"search_results_6": {}},
    "20261009"
  ])");
  const Json::Value days = tapo_recordings::days(results);
  REQUIRE(days.size() == 2);
  CHECK(days[0].asString() == "2026-10-01");
  CHECK(days[1].asString() == "2026-10-12");
  CHECK(tapo_recordings::days(Json::Value()).empty());
}

TEST_CASE("a day's recordings become sorted segments, numbers or numeric strings, and nothing impossible")
{
  const Json::Value results = parsed(R"([
    {"search_video_results_1": {"startTime": 1790000300, "endTime": 1790000600, "vod_type": 1}},
    {"search_video_results_2": {"startTime": "1790000000", "endTime": "1790000290"}},
    {"search_video_results_3": {"startTime": 1790000900, "endTime": 1790000800}},
    {"search_video_results_4": {"startTime": "soon", "endTime": 1790000900}},
    {"startTime": 1790001000, "endTime": 1790001000}
  ])");
  const Json::Value segments = tapo_recordings::segments(results);
  REQUIRE(segments.size() == 3);
  CHECK(segments[0]["start"].asInt64() == 1790000000);
  CHECK(segments[0]["end"].asInt64() == 1790000290);
  CHECK(segments[1]["start"].asInt64() == 1790000300);
  CHECK(segments[2]["start"].asInt64() == 1790001000);
  CHECK(segments[2]["end"].asInt64() == 1790001000);
  CHECK_FALSE(segments[1].isMember("vod_type"));
}

TEST_CASE("the SD day list asks channel 0 between the two dates and answers ISO days")
{
  tapo_control_hub::reset();
  auto camera = std::make_shared<ScriptedCamera>();
  camera->answers["searchDateWithVideo"] =
      parsed(R"({"playback": {"search_results": [{"search_results_1": {"date": "20261012"}}]}})");
  TapoApi api(scriptedConfig(camera));
  const TapoResult days = api.recordingDays({.from = "20261001", .to = "20261031"});
  REQUIRE(days.ok);
  REQUIRE(days.data["days"].size() == 1);
  CHECK(days.data["days"][0].asString() == "2026-10-12");
  REQUIRE(camera->asked.size() == 1);
  const Json::Value& search = camera->asked.back()["params"]["playback"]["search_year_utility"];
  CHECK(camera->asked.back()["method"].asString() == "searchDateWithVideo");
  CHECK(search["start_date"].asString() == "20261001");
  CHECK(search["end_date"].asString() == "20261031");
  REQUIRE(search["channel"].isArray());
  CHECK(search["channel"][0].asInt() == 0);
}

TEST_CASE("a day's recordings are asked for with the camera's own user id")
{
  tapo_control_hub::reset();
  auto camera = std::make_shared<ScriptedCamera>();
  camera->answers["getUserID"] = parsed(R"({"user_id": 8})");
  camera->answers["searchVideoOfDay"] = parsed(R"({"playback": {"search_video_results": [
    {"search_video_results_1": {"startTime": 1790000300, "endTime": 1790000600}}]}})");
  TapoApi api(scriptedConfig(camera));
  const TapoResult day = api.recordingsOfDay({.date = "20261012"});
  REQUIRE(day.ok);
  REQUIRE(day.data["segments"].size() == 1);
  CHECK(day.data["segments"][0]["end"].asInt64() == 1790000600);
  REQUIRE(camera->asked.size() == 2);
  CHECK(camera->asked[0]["method"].asString() == "getUserID");
  const Json::Value& search = camera->asked[1]["params"]["playback"]["search_video_utility"];
  CHECK(camera->asked[1]["method"].asString() == "searchVideoOfDay");
  CHECK(search["date"].asString() == "20261012");
  CHECK(search["id"].asInt64() == 8);
  CHECK(search["channel"].asInt() == 0);
  CHECK(search["start_index"].asInt() == 0);
}

TEST_CASE("a camera that refuses or cannot search its card says so")
{
  tapo_control_hub::reset();
  auto refusing = std::make_shared<ScriptedCamera>();
  refusing->refusals["searchDateWithVideo"] = -40106;
  TapoApi refused(scriptedConfig(refusing));
  const TapoResult denied = refused.recordingDays({.from = "20261001", .to = "20261031"});
  CHECK_FALSE(denied.ok);
  CHECK(denied.errorCode == -40106);

  tapo_control_hub::reset();
  auto silent = std::make_shared<ScriptedCamera>();
  silent->answers["getUserID"] = parsed(R"({"user_id": 8})");
  TapoApi quiet(scriptedConfig(silent));
  CHECK_FALSE(quiet.recordingDays({.from = "20261001", .to = "20261031"}).ok);
  CHECK_FALSE(quiet.recordingsOfDay({.date = "20261012"}).ok);

  tapo_control_hub::reset();
  auto anonymous = std::make_shared<ScriptedCamera>();
  TapoApi unnamed(scriptedConfig(anonymous));
  CHECK_FALSE(unnamed.recordingsOfDay({.date = "20261012"}).ok);
  REQUIRE(anonymous->asked.size() == 1);
  CHECK(anonymous->asked[0]["method"].asString() == "getUserID");
}
