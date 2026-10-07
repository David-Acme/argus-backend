#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <shared/services/tapo/tapo-client.hxx>
#include <shared/services/tapo/tapo-control-hub.hxx>
#include <shared/services/tapo/tapo-lockout.hxx>
#include <string>
#include <utility>
#include <vector>

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
  TapoTransportKind kind() const override { return request_.kind; }
  Json::Value state() const override { return Json::Value(Json::objectValue); }
  bool isAuthenticated() const override { return authenticated_; }

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
  std::size_t logins() const { return ledger->logins.size(); }
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
  CHECK(reading->secLeft == 1650);
  CHECK(reading->code == -40404);

  Json::Value flat(Json::objectValue);
  flat["data"]["code"] = -40404;
  flat["data"]["sec_left"] = "68";
  flat["error_code"] = -40401;
  const auto flatReading = tapo_lockout::of(flat);
  REQUIRE(flatReading.has_value());
  CHECK(flatReading->secLeft == 68);

  Json::Value refused(Json::objectValue);
  refused["error_code"] = -40401;
  CHECK_FALSE(tapo_lockout::of(refused).has_value());
}
