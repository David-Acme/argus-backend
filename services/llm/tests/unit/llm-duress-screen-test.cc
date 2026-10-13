#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/duress/services/duress-feed.hxx>
#include <feature/duress/services/duress-screen.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <sync/resource-change.hxx>
#include <text/json-util.hxx>
#include <text/secret-phrase.hxx>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::string sealed(std::string_view phrase)
{
  std::string salt;
  for (char value = 0; value < 16; ++value)
    salt.push_back(static_cast<char>(value + 1));
  const auto key = secret_phrase::seal({.phrase = phrase, .salt = salt, .iterations = secret_phrase::kMinIterations});
  REQUIRE(key.has_value());
  return secret_phrase::encode(key.value_or(secret_phrase::PhraseKey{}));
}

class ScriptedLink final : public DuressLink
{
public:
  explicit ScriptedLink(std::optional<std::string> key) : key_(std::move(key)) {}

  [[nodiscard]] std::optional<std::string> phraseKey() const override
  {
    const std::scoped_lock lock(mutex_);
    ++asked_;
    return key_;
  }

  [[nodiscard]] DuressDelivery raise(const DuressRaise& raise) const override
  {
    const std::scoped_lock lock(mutex_);
    attempts_.push_back(raise);
    if (replies_.empty())
      return DuressDelivery::Delivered;
    const DuressDelivery reply = replies_.front();
    replies_.pop_front();
    return reply;
  }

  void setKey(std::optional<std::string> key)
  {
    const std::scoped_lock lock(mutex_);
    key_ = std::move(key);
  }

  void reply(std::deque<DuressDelivery> replies)
  {
    const std::scoped_lock lock(mutex_);
    replies_ = std::move(replies);
  }

  [[nodiscard]] std::vector<DuressRaise> attempts() const
  {
    const std::scoped_lock lock(mutex_);
    return attempts_;
  }

  [[nodiscard]] int asked() const
  {
    const std::scoped_lock lock(mutex_);
    return asked_;
  }

private:
  mutable std::mutex mutex_;
  std::optional<std::string> key_;
  mutable std::deque<DuressDelivery> replies_;
  mutable std::vector<DuressRaise> attempts_;
  mutable int asked_{0};
};

bool disarmer(UserRole role)
{
  return role == UserRole::Owner || role == UserRole::Resident;
}

DuressScreenTiming quick()
{
  return {.refresh = std::chrono::minutes(1),
          .retryBase = std::chrono::milliseconds(10),
          .retryMax = std::chrono::milliseconds(40),
          .queueLimit = 8};
}

bool eventually(const std::function<bool()>& condition)
{
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return condition();
}

struct ScreenRig
{
  explicit ScreenRig(std::optional<std::string> key)
      : link(std::make_shared<ScriptedLink>(std::move(key))),
        screen({.link = link, .eligible = disarmer}, quick())
  {
    screen.start();
    REQUIRE(eventually([this] { return link->asked() > 0 && screen.idle(); }));
  }

  void say(int64_t userId, UserRole role, std::string_view text) const
  {
    screen.screen({.userId = userId, .role = role, .text = text});
  }

  std::shared_ptr<ScriptedLink> link;
  DuressScreen screen;
};

constexpr std::string_view kPhrase = "El gato está en el tejado";

struct Seen
{
  int64_t userId{0};
  UserRole role{UserRole::Unknown};
  std::string text;
};

class RecordingScreen final : public turn::UtteranceScreen
{
public:
  void screen(const turn::ScreenedUtterance& utterance) const override
  {
    seen_.push_back({.userId = utterance.userId, .role = utterance.role, .text = std::string(utterance.text)});
  }

  [[nodiscard]] const std::vector<Seen>& seen() const { return seen_; }

private:
  mutable std::vector<Seen> seen_;
};

ChatRequest turnOf(std::vector<ChatMessage> messages)
{
  ChatRequest request;
  request.messages = std::move(messages);
  request.userId = 2;
  request.role = UserRole::Resident;
  return request;
}

}

TEST_CASE("a household member who says the phrase raises the alert once, with the key it matched")
{
  const std::string key = sealed(kPhrase);
  ScreenRig rig(key);
  rig.say(2, UserRole::Resident, "Argus, ¿sabes qué? El gato está en el tejado.");
  REQUIRE(eventually([&rig] { return rig.link->attempts().size() == 1 && rig.screen.idle(); }));
  const DuressRaise raised = rig.link->attempts().front();
  CHECK(raised.userId == 2);
  CHECK(raised.role == UserRole::Resident);
  CHECK(raised.key == key);
}

TEST_CASE("nobody else raises it: other words, a guest, an anonymous caller")
{
  ScreenRig rig(sealed(kPhrase));
  rig.say(2, UserRole::Resident, "el perro está en el tejado");
  rig.say(3, UserRole::Guest, "el gato está en el tejado");
  rig.say(0, UserRole::Owner, "el gato está en el tejado");
  rig.say(4, UserRole::Unknown, "el gato está en el tejado");
  CHECK(eventually([&rig] { return rig.screen.idle(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(rig.link->attempts().empty());
}

TEST_CASE("a raise guard did not take is retried until it is, and only once per speaker")
{
  ScreenRig rig(sealed(kPhrase));
  rig.link->reply({DuressDelivery::Retry, DuressDelivery::Retry});
  rig.say(2, UserRole::Owner, "el gato está en el tejado");
  rig.say(2, UserRole::Owner, "repito: el gato está en el tejado");
  REQUIRE(eventually([&rig] { return rig.link->attempts().size() >= 3 && rig.screen.idle(); }));
  CHECK(rig.link->attempts().size() == 3);
}

TEST_CASE("without a phrase nothing is queued or raised")
{
  ScreenRig rig(std::string{});
  rig.say(2, UserRole::Resident, "el gato está en el tejado");
  CHECK(eventually([&rig] { return rig.screen.idle(); }));
  CHECK(rig.link->attempts().empty());
}

TEST_CASE("a refreshed phrase replaces the old one")
{
  ScreenRig rig(sealed(kPhrase));
  rig.link->setKey(sealed("la tía Rosa no viene"));
  const int asked = rig.link->asked();
  rig.screen.requestRefresh();
  REQUIRE(eventually([&rig, asked] { return rig.link->asked() > asked && rig.screen.idle(); }));
  rig.say(2, UserRole::Resident, "el gato está en el tejado");
  CHECK(eventually([&rig] { return rig.screen.idle(); }));
  CHECK(rig.link->attempts().empty());
  rig.say(2, UserRole::Resident, "dile que la tía Rosa no viene hoy");
  CHECK(eventually([&rig] { return rig.link->attempts().size() == 1; }));
}

TEST_CASE("what was said while guard was unreachable is screened once it answers")
{
  const auto link = std::make_shared<ScriptedLink>(std::nullopt);
  DuressScreen screen({.link = link, .eligible = disarmer}, quick());
  screen.start();
  REQUIRE(eventually([&link] { return link->asked() > 0; }));
  screen.screen({.userId = 2, .role = UserRole::Owner, .text = "el gato está en el tejado"});
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(link->attempts().empty());
  link->setKey(sealed(kPhrase));
  REQUIRE(eventually([&link] { return link->attempts().size() == 1; }));
  CHECK(link->attempts().front().userId == 2);
}

TEST_CASE("the turn hook screens only an identified caller's spoken turn")
{
  RecordingScreen recorded;
  const std::vector<ChatMessage> history{{.role = "system", .content = "Eres Argus."},
                                         {.role = "user", .content = "hola"},
                                         {.role = "assistant", .content = "Hola, ¿qué tal?"},
                                         {.role = "user", .content = "el gato está en el tejado"}};
  screenTurn(&recorded, turnOf(history));
  REQUIRE(recorded.seen().size() == 1);
  CHECK(recorded.seen().front().userId == 2);
  CHECK(recorded.seen().front().role == UserRole::Resident);
  CHECK(recorded.seen().front().text == "el gato está en el tejado");

  ChatRequest anonymous = turnOf(history);
  anonymous.userId = 0;
  screenTurn(&recorded, anonymous);
  ChatRequest unknown = turnOf(history);
  unknown.role = UserRole::Unknown;
  screenTurn(&recorded, unknown);
  ChatRequest prefill = turnOf(history);
  prefill.prefillOnly = true;
  screenTurn(&recorded, prefill);
  screenTurn(nullptr, turnOf(history));
  CHECK(recorded.seen().size() == 1);
}

TEST_CASE("only guard's duress notice asks for a refresh")
{
  const std::string duress =
      json_util::toString(resource_change::payload(ResourceDomain::Guard, ResourceScope::Duress, {}));
  const std::string pin =
      json_util::toString(resource_change::payload(ResourceDomain::Guard, ResourceScope::SafetyPin, {}, 2));
  CHECK(duress_feed::changesPhrase(duress));
  CHECK_FALSE(duress_feed::changesPhrase(pin));
  CHECK_FALSE(duress_feed::changesPhrase("not json"));
  CHECK_FALSE(duress_feed::changesPhrase("{}"));
}
