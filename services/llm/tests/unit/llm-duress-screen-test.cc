#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <feature/duress/services/duress-feed.hxx>
#include <feature/duress/services/duress-screen.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <sync/resource-change.hxx>
#include <text/json-util.hxx>
#include <text/secret-phrase.hxx>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif

namespace
{

constexpr std::string_view kPhrase = "El gato está en el tejado";
constexpr std::string_view kStatement = "a mi hermana le gusta el café negro";
constexpr const char* kScratchDir = "/tmp/argus-llm-duress-screen-test";

std::string sealed(std::string_view phrase)
{
  std::string salt;
  for (char value = 0; value < 16; ++value)
    salt.push_back(static_cast<char>(value + 1));
  const auto key = secret_phrase::seal({.phrase = phrase, .salt = salt, .iterations = secret_phrase::kMinIterations});
  REQUIRE(key.has_value());
  return secret_phrase::encode(key.value_or(secret_phrase::PhraseKey{}));
}

bool traceOf(std::string_view phrase, std::string_view text)
{
  return secret_phrase::spokenIn(secret_phrase::decode(sealed(phrase)).value_or(secret_phrase::PhraseKey{}), text);
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
          .memo = std::chrono::minutes(2),
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

  [[nodiscard]] turn::ScreenedText say(int64_t userId, UserRole role, std::string_view text) const
  {
    return screen.screen({.userId = userId, .role = role, .text = text});
  }

  std::shared_ptr<ScriptedLink> link;
  DuressScreen screen;
};

ChatRequest turnOf(std::vector<ChatMessage> messages)
{
  ChatRequest request;
  request.messages = std::move(messages);
  request.userId = 2;
  request.role = UserRole::Resident;
  request.lang = "es";
  return request;
}

std::vector<ChatMessage> historyEndingWith(std::string_view utterance)
{
  return {{.role = "system", .content = "Eres Argus."},
          {.role = "user", .content = "hola"},
          {.role = "assistant", .content = "Hola, ¿qué tal?"},
          {.role = "user", .content = std::string(utterance)}};
}

struct ScriptedEngine
{
  std::vector<ChatRequest> requests;
  std::string reply{"¿Perdona? No te he entendido."};

  ChatEngine engine()
  {
    return {.chat =
                [this](const ChatRequest& request) {
                  requests.push_back(request);
                  return reply;
                },
            .chatStream =
                [this](const ChatRequest& request, const TokenCallback& onToken) {
                  requests.push_back(request);
                  onToken(reply, false);
                  onToken("", true);
                }};
  }
};

class SilentChat final : public IMemoryChat
{
public:
  [[nodiscard]] bool available() const override { return false; }
  [[nodiscard]] bool busy() const override { return false; }
  [[nodiscard]] std::string chat(const ChatRequest&) const override { return {}; }
};

std::string scratchDb(const std::string& name)
{
  std::filesystem::create_directories(kScratchDir);
  const std::string db = std::string(kScratchDir) + "/" + name;
  std::filesystem::remove(db);
  const std::string config = std::string(kScratchDir) + "/" + name + ".toml";
  std::ofstream out(config);
  out << "[database]\nfile = \"" << db << "\"\n"
      << "[memory]\n"
      << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
      << "catalog_person_table = \"catalog_person\"\n"
      << "catalog_camera_table = \"catalog_camera\"\n"
      << "catalog_zone_table = \"catalog_zone\"\n"
      << "catalog_stream_table = \"catalog_stream\"\n"
      << "create_face_vec = false\n"
      << "embedding_model = \"" << kScratchDir << "/none.onnx\"\n"
      << "embedding_tokenizer = \"" << kScratchDir << "/none.json\"\n"
      << "embedding_preload = false\n"
      << "[extract]\n"
      << "model_path = \"" << kScratchDir << "/none.gguf\"\n";
  out.close();
  ConfigService::load(config);
  return db;
}

std::vector<std::string> graphTexts(const std::string& db)
{
  std::vector<std::string> texts;
  sqlite3* handle = nullptr;
  if (sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(handle);
    return texts;
  }
  for (const char* sql : {"SELECT canonical FROM memory_entity", "SELECT surface || ' ' || norm FROM memory_alias",
                          "SELECT predicate || ' ' || value || ' ' || canonical FROM memory_fact",
                          "SELECT summary FROM memory_episode"}) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) != SQLITE_OK)
      continue;
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const unsigned char* text = sqlite3_column_text(statement, 0);
      if (text != nullptr)
        texts.emplace_back(reinterpret_cast<const char*>(text));
    }
    sqlite3_finalize(statement);
  }
  sqlite3_close(handle);
  return texts;
}

tools::ToolResult rememberAsTheTurnWould(MemoryService& memory, const ChatRequest& turn)
{
  const std::string said = LfmAdapter::lastUtterance(turn.messages);
  tools::ToolCall call;
  call.name = "memory.remember";
  call.arguments = Json::Value(Json::objectValue);
  call.arguments["text"] = said;
  call.context.userId = turn.userId;
  call.context.role = turn.role;
  call.context.lang = "es";
  call.context.channel = "tool_result";
  call.context.utterance = said;
  call.context.decided = true;
  for (const auto& descriptor : memory.toolDescriptors())
    if (descriptor.spec.name == "memory.remember")
      return descriptor.handler(call);
  FAIL("memory.remember is not served");
  return {};
}

}

TEST_CASE("a household member who says the phrase is withheld at once and the alert carries the key it matched")
{
  const std::string key = sealed(kPhrase);
  ScreenRig rig(key);
  const turn::ScreenedText heard = rig.say(2, UserRole::Resident, "Argus, ¿sabes qué? El gato está en el tejado.");
  CHECK(heard.withheld);
  CHECK(heard.text == turn::kWithheldUtterance);
  REQUIRE(eventually([&rig] { return rig.link->attempts().size() == 1 && rig.screen.idle(); }));
  const DuressRaise raised = rig.link->attempts().front();
  CHECK(raised.userId == 2);
  CHECK(raised.role == UserRole::Resident);
  CHECK(raised.key == key);
}

TEST_CASE("nobody else is withheld: other words, a guest, an anonymous caller, an unknown role")
{
  ScreenRig rig(sealed(kPhrase));
  CHECK_FALSE(rig.say(2, UserRole::Resident, "el perro está en el tejado").withheld);
  CHECK(rig.say(2, UserRole::Resident, "el perro está en el tejado").text == "el perro está en el tejado");
  CHECK_FALSE(rig.say(3, UserRole::Guest, "el gato está en el tejado").withheld);
  CHECK_FALSE(rig.say(0, UserRole::Owner, "el gato está en el tejado").withheld);
  CHECK_FALSE(rig.say(4, UserRole::Unknown, "el gato está en el tejado").withheld);
  CHECK(eventually([&rig] { return rig.screen.idle(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(rig.link->attempts().empty());
}

TEST_CASE("a raise guard did not take is retried until it is, and only once per speaker")
{
  ScreenRig rig(sealed(kPhrase));
  rig.link->reply({DuressDelivery::Retry, DuressDelivery::Retry});
  CHECK(rig.say(2, UserRole::Owner, "el gato está en el tejado").withheld);
  CHECK(rig.say(2, UserRole::Owner, "repito: el gato está en el tejado").withheld);
  REQUIRE(eventually([&rig] { return rig.link->attempts().size() >= 3 && rig.screen.idle(); }));
  CHECK(rig.link->attempts().size() == 3);
}

TEST_CASE("without a phrase nothing is withheld or raised")
{
  ScreenRig rig(std::string{});
  CHECK_FALSE(rig.say(2, UserRole::Resident, "el gato está en el tejado").withheld);
  CHECK(eventually([&rig] { return rig.screen.idle(); }));
  CHECK(rig.link->attempts().empty());
}

TEST_CASE("a refreshed phrase replaces the old one, and a remembered clean answer never hides it")
{
  ScreenRig rig(sealed(kPhrase));
  CHECK_FALSE(rig.say(2, UserRole::Resident, "dile que la tía Rosa no viene hoy").withheld);
  rig.link->setKey(sealed("la tía Rosa no viene"));
  const int asked = rig.link->asked();
  rig.screen.requestRefresh();
  REQUIRE(eventually([&rig, asked] { return rig.link->asked() > asked && rig.screen.idle(); }));
  CHECK_FALSE(rig.say(2, UserRole::Resident, "el gato está en el tejado").withheld);
  CHECK(rig.say(2, UserRole::Resident, "dile que la tía Rosa no viene hoy").withheld);
  CHECK(eventually([&rig] { return rig.link->attempts().size() == 1; }));
}

TEST_CASE("what is said before guard first answers is not withheld, and still raises once guard answers")
{
  const auto link = std::make_shared<ScriptedLink>(std::nullopt);
  DuressScreen screen({.link = link, .eligible = disarmer}, quick());
  screen.start();
  REQUIRE(eventually([&link] { return link->asked() > 0; }));
  CHECK_FALSE(screen.screen({.userId = 2, .role = UserRole::Owner, .text = "el gato está en el tejado"}).withheld);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(link->attempts().empty());
  link->setKey(sealed(kPhrase));
  REQUIRE(eventually([&link] { return link->attempts().size() == 1; }));
  CHECK(link->attempts().front().userId == 2);
}

TEST_CASE("the turn carries the placeholder in place of the phrase and keeps the rest of its history")
{
  ScreenRig rig(sealed(kPhrase));
  const ChatRequest screened = screenTurn(&rig.screen, turnOf(historyEndingWith("Argus, el gato está en el tejado")));
  REQUIRE(screened.messages.size() == 4);
  CHECK(screened.messages[0].content == "Eres Argus.");
  CHECK(screened.messages[1].content == "hola");
  CHECK(screened.messages[2].content == "Hola, ¿qué tal?");
  CHECK(screened.messages[3].role == "user");
  CHECK(screened.messages[3].content == turn::kWithheldUtterance);
  for (const ChatMessage& message : screened.messages)
    CHECK_FALSE(traceOf(kPhrase, message.content));

  const ChatRequest plain = screenTurn(&rig.screen, turnOf(historyEndingWith("¿qué hora es?")));
  CHECK(plain.messages.back().content == "¿qué hora es?");

  ChatRequest anonymous = turnOf(historyEndingWith("el gato está en el tejado"));
  anonymous.userId = 0;
  CHECK(screenTurn(&rig.screen, anonymous).messages.back().content == "el gato está en el tejado");
  ChatRequest prefill = turnOf(historyEndingWith("el gato está en el tejado"));
  prefill.prefillOnly = true;
  CHECK(screenTurn(&rig.screen, prefill).messages.back().content == "el gato está en el tejado");
  CHECK(screenTurn(nullptr, turnOf(historyEndingWith("el gato está en el tejado"))).messages.back().content ==
        "el gato está en el tejado");
}

TEST_CASE("the model is never prompted with the phrase, and the reply stays an ordinary one")
{
  ScreenRig rig(sealed(kPhrase));
  ToolRegistry registry;
  ScriptedEngine engine;
  LfmAdapter adapter({.engine = engine.engine(), .registry = registry, .router = nullptr});
  const ToolChatOutput answer =
      adapter.chatPlain(screenTurn(&rig.screen, turnOf(historyEndingWith("el gato está en el tejado"))));
  REQUIRE(engine.requests.size() == 1);
  for (const ChatMessage& message : engine.requests.front().messages)
    CHECK_FALSE(traceOf(kPhrase, message.content));
  CHECK(answer.reply == "¿Perdona? No te he entendido.");
  CHECK(answer.reply.find("coacción") == std::string::npos);
}

TEST_CASE("the memory graph keeps the same words when they are not the phrase")
{
  const std::string db = scratchDb("unscreened.db");
  SilentChat chat;
  MemoryService memory(VecDb::instance(), chat);
  memory.init({});
  ScreenRig rig(sealed(kPhrase));
  const tools::ToolResult stored =
      rememberAsTheTurnWould(memory, screenTurn(&rig.screen, turnOf(historyEndingWith(kStatement))));
  CHECK(stored.ok);
  memory.shutdown();
  const auto texts = graphTexts(db);
  CHECK(std::ranges::any_of(texts, [](const std::string& text) { return text.find("caf") != std::string::npos; }));
}

TEST_CASE("the memory graph keeps no trace of the phrase once it is withheld")
{
  const std::string db = scratchDb("screened.db");
  SilentChat chat;
  MemoryService memory(VecDb::instance(), chat);
  memory.init({});
  ScreenRig rig(sealed(kStatement));
  const ChatRequest screened = screenTurn(&rig.screen, turnOf(historyEndingWith(kStatement)));
  CHECK(screened.messages.back().content == turn::kWithheldUtterance);
  const tools::ToolResult stored = rememberAsTheTurnWould(memory, screened);
  CHECK_FALSE(stored.ok);
  memory.shutdown();
  for (const std::string& text : graphTexts(db)) {
    CHECK(text.find("caf") == std::string::npos);
    CHECK(text.find("hermana") == std::string::npos);
  }
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
