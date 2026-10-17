#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <feature/duress/infra/sqlite-duress-outbox.hxx>
#include <feature/duress/services/duress-feed.hxx>
#include <feature/duress/services/duress-screen.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <llm/llm-speech.hxx>
#include <sqlite/vec-db.hxx>
#include <sqlite3.h>
#include <sync/resource-change.hxx>
#include <text/json-util.hxx>
#include <text/secret-phrase.hxx>
#include <text/sha256.hxx>

#include <algorithm>
#include <atomic>
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
#include <tuple>
#include <utility>
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
    if (unreachable_)
      return DuressDelivery::Retry;
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

  void setUnreachable(bool unreachable)
  {
    const std::scoped_lock lock(mutex_);
    unreachable_ = unreachable;
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
  bool unreachable_{false};
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
  for (const char* suffix : {"", "-wal", "-shm"})
    std::filesystem::remove(db + suffix);
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

struct OutboxRow
{
  int64_t userId{0};
  std::string role;
  std::string keyDigest;
  int64_t saidAt{0};
};

std::vector<OutboxRow> outboxRows(const std::string& db)
{
  std::vector<OutboxRow> rows;
  sqlite3* handle = nullptr;
  if (sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(handle);
    return rows;
  }
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(handle, "SELECT user_id, role, key_digest, said_at FROM duress_raise_outbox ORDER BY id", -1,
                         &statement, nullptr) == SQLITE_OK) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const auto* role = sqlite3_column_text(statement, 1);
      const auto* digest = sqlite3_column_text(statement, 2);
      rows.push_back({.userId = sqlite3_column_int64(statement, 0),
                      .role = role != nullptr ? reinterpret_cast<const char*>(role) : "",
                      .keyDigest = digest != nullptr ? reinterpret_cast<const char*>(digest) : "",
                      .saidAt = sqlite3_column_int64(statement, 3)});
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(handle);
  return rows;
}

void backdate(const std::string& db, int64_t saidAt)
{
  sqlite3* handle = nullptr;
  REQUIRE(sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  REQUIRE(sqlite3_prepare_v2(handle, "UPDATE duress_raise_outbox SET said_at = ?", -1, &statement, nullptr) ==
          SQLITE_OK);
  sqlite3_bind_int64(statement, 1, saidAt);
  CHECK(sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(handle);
}

int pragmaOf(sqlite3* db, const char* pragma)
{
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, pragma, -1, &statement, nullptr) != SQLITE_OK)
    return -1;
  const int value = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  return value;
}

struct ManualClock
{
  std::atomic<int64_t> seconds{1'790'000'000};

  [[nodiscard]] std::function<int64_t()> source()
  {
    return [this] { return seconds.load(); };
  }
};

std::shared_ptr<DuressOutbox> outboxOver(SqliteGraph& graph)
{
  return std::make_shared<SqliteDuressOutbox>(
      SqliteDuressOutboxInput{.mutex = &graph.mutex(), .db = [&graph] { return graph.handle(); }});
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
  CHECK(heard.text == llm_speech::kWithheldUtterance);
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
  const ScreenedRequest verdict = screenTurn(&rig.screen, turnOf(historyEndingWith("Argus, el gato está en el tejado")));
  CHECK(verdict.withheld);
  const ChatRequest& screened = verdict.request;
  REQUIRE(screened.messages.size() == 4);
  CHECK(screened.messages[0].content == "Eres Argus.");
  CHECK(screened.messages[1].content == "hola");
  CHECK(screened.messages[2].content == "Hola, ¿qué tal?");
  CHECK(screened.messages[3].role == "user");
  CHECK(screened.messages[3].content == llm_speech::kWithheldUtterance);
  for (const ChatMessage& message : screened.messages)
    CHECK_FALSE(traceOf(kPhrase, message.content));

  const ScreenedRequest plain = screenTurn(&rig.screen, turnOf(historyEndingWith("¿qué hora es?")));
  CHECK_FALSE(plain.withheld);
  CHECK(plain.request.messages.back().content == "¿qué hora es?");

  ChatRequest anonymous = turnOf(historyEndingWith("el gato está en el tejado"));
  anonymous.userId = 0;
  CHECK(screenTurn(&rig.screen, anonymous).request.messages.back().content == "el gato está en el tejado");
  ChatRequest prefill = turnOf(historyEndingWith("el gato está en el tejado"));
  prefill.prefillOnly = true;
  CHECK(screenTurn(&rig.screen, prefill).request.messages.back().content == "el gato está en el tejado");
  CHECK(screenTurn(nullptr, turnOf(historyEndingWith("el gato está en el tejado"))).request.messages.back().content ==
        "el gato está en el tejado");
}

TEST_CASE("the model is never prompted with the phrase")
{
  ScreenRig rig(sealed(kPhrase));
  ToolRegistry registry;
  ScriptedEngine engine;
  LfmAdapter adapter({.engine = engine.engine(), .registry = registry});
  std::ignore = adapter.chatPlain(screenTurn(&rig.screen, turnOf(historyEndingWith("el gato está en el tejado"))).request);
  REQUIRE(engine.requests.size() == 1);
  for (const ChatMessage& message : engine.requests.front().messages)
    CHECK_FALSE(traceOf(kPhrase, message.content));
}

TEST_CASE("a withheld turn reaches no model: the stream ends at once with the unheard marker")
{
  ScreenRig rig(sealed(kPhrase));
  LlmController controller;
  controller.useScreen(rig.screen);
  std::vector<std::pair<std::string, bool>> tokens;
  std::string speech;
  std::string act;
  controller.chatStreamSync({.request = turnOf(historyEndingWith("el gato está en el tejado")),
                             .onToken = [&tokens](const std::string& token, bool done) { tokens.emplace_back(token, done); },
                             .stats = nullptr,
                             .cancellation = {},
                             .onAction = {},
                             .speech = &speech,
                             .act = &act});
  REQUIRE(tokens.size() == 1);
  CHECK(tokens.front().first.empty());
  CHECK(tokens.front().second);
  CHECK(speech == llm_speech::kUnheard);
  CHECK(act.empty());

  const LlmChatOutcome unary = controller.chatSync(turnOf(historyEndingWith("el gato está en el tejado")));
  CHECK(unary.text.empty());
  CHECK(unary.speech == llm_speech::kUnheard);
  CHECK(unary.hops == 0);
  CHECK(unary.toolCalls == 0);
}

TEST_CASE("the memory graph keeps the same words when they are not the phrase")
{
  const std::string db = scratchDb("unscreened.db");
  SilentChat chat;
  MemoryService memory(VecDb::instance(), chat);
  memory.init({});
  ScreenRig rig(sealed(kPhrase));
  const tools::ToolResult stored =
      rememberAsTheTurnWould(memory, screenTurn(&rig.screen, turnOf(historyEndingWith(kStatement))).request);
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
  const ChatRequest screened = screenTurn(&rig.screen, turnOf(historyEndingWith(kStatement))).request;
  CHECK(screened.messages.back().content == llm_speech::kWithheldUtterance);
  const tools::ToolResult stored = rememberAsTheTurnWould(memory, screened);
  CHECK_FALSE(stored.ok);
  memory.shutdown();
  for (const std::string& text : graphTexts(db)) {
    CHECK(text.find("caf") == std::string::npos);
    CHECK(text.find("hermana") == std::string::npos);
  }
}

TEST_CASE("a match is written down before the turn goes on, as the key's digest and never the words")
{
  const std::string db = scratchDb("outbox-written.db");
  SqliteGraph graph;
  REQUIRE(graph.open(db));
  const std::string key = sealed(kPhrase);
  const auto link = std::make_shared<ScriptedLink>(key);
  link->setUnreachable(true);
  ManualClock clock;
  DuressScreen screen({.link = link, .eligible = disarmer, .outbox = outboxOver(graph), .clock = clock.source()},
                      quick());
  screen.start();
  REQUIRE(eventually([&link, &screen] { return link->asked() > 0 && screen.idle(); }));

  CHECK(screen.screen({.userId = 2, .role = UserRole::Resident, .text = "oye, el gato está en el tejado"}).withheld);
  const std::vector<OutboxRow> written = outboxRows(db);
  REQUIRE(written.size() == 1);
  CHECK(written.front().userId == 2);
  CHECK(written.front().role == "resident");
  CHECK(written.front().keyDigest == argus::hash::sha256Hex(key));
  CHECK(written.front().keyDigest != key);
  CHECK(written.front().saidAt == 1'790'000'000);
  for (const std::string_view word : {"gato", "tejado", "oye"}) {
    CHECK(written.front().role.find(word) == std::string::npos);
    CHECK(written.front().keyDigest.find(word) == std::string::npos);
  }

  CHECK(screen.screen({.userId = 2, .role = UserRole::Resident, .text = "el gato está en el tejado"}).withheld);
  CHECK(outboxRows(db).size() == 1);

  REQUIRE(eventually([&link] { return !link->attempts().empty(); }));
  link->setUnreachable(false);
  link->reply({DuressDelivery::Dropped});
  REQUIRE(eventually([&db, &screen] { return outboxRows(db).empty() && screen.idle(); }));
  screen.requestStop();
}

TEST_CASE("a raise written down survives a restart of argus-llm and reaches guard once guard takes it")
{
  const std::string db = scratchDb("outbox-restart.db");
  const std::string key = sealed(kPhrase);
  {
    SqliteGraph graph;
    REQUIRE(graph.open(db));
    const auto unreachable = std::make_shared<ScriptedLink>(key);
    unreachable->setUnreachable(true);
    DuressScreen before({.link = unreachable, .eligible = disarmer, .outbox = outboxOver(graph)}, quick());
    before.start();
    REQUIRE(eventually([&unreachable, &before] { return unreachable->asked() > 0 && before.idle(); }));
    CHECK(before.screen({.userId = 3, .role = UserRole::Owner, .text = "el gato está en el tejado"}).withheld);
    REQUIRE(eventually([&unreachable] { return unreachable->attempts().size() >= 2; }));
  }
  const std::vector<OutboxRow> carried = outboxRows(db);
  REQUIRE(carried.size() == 1);
  CHECK(carried.front().userId == 3);
  CHECK(carried.front().role == "owner");
  backdate(db, 1'700'000'000);

  SqliteGraph graph;
  REQUIRE(graph.open(db));
  const auto guard = std::make_shared<ScriptedLink>(key);
  DuressScreen after({.link = guard, .eligible = disarmer, .outbox = outboxOver(graph)}, quick());
  after.start();
  REQUIRE(eventually([&guard, &after, &db] { return guard->attempts().size() == 1 && after.idle() && outboxRows(db).empty(); }));
  const DuressRaise raised = guard->attempts().front();
  CHECK(raised.userId == 3);
  CHECK(raised.role == UserRole::Owner);
  CHECK(raised.key == key);
  CHECK(raised.saidAt == 1'700'000'000);
  after.requestStop();
}

TEST_CASE("the raise carries the instant the words reached the screen, and every retry carries the same")
{
  const auto link = std::make_shared<ScriptedLink>(sealed(kPhrase));
  link->reply({DuressDelivery::Retry, DuressDelivery::Retry});
  ManualClock clock;
  DuressScreen screen({.link = link, .eligible = disarmer, .outbox = nullptr, .clock = clock.source()}, quick());
  screen.start();
  REQUIRE(eventually([&link, &screen] { return link->asked() > 0 && screen.idle(); }));
  clock.seconds = 1'790'000'100;
  CHECK(screen.screen({.userId = 2, .role = UserRole::Resident, .text = "el gato está en el tejado"}).withheld);
  clock.seconds = 1'790'009'999;
  REQUIRE(eventually([&link, &screen] { return link->attempts().size() == 3 && screen.idle(); }));
  for (const DuressRaise& attempt : link->attempts())
    CHECK(attempt.saidAt == 1'790'000'100);
  screen.requestStop();
}

TEST_CASE("words matched once guard answers carry the time they were heard, not the time they matched")
{
  const auto link = std::make_shared<ScriptedLink>(std::nullopt);
  ManualClock clock;
  DuressScreen screen({.link = link, .eligible = disarmer, .outbox = nullptr, .clock = clock.source()}, quick());
  screen.start();
  REQUIRE(eventually([&link] { return link->asked() > 0; }));
  clock.seconds = 1'790'000'200;
  CHECK_FALSE(screen.screen({.userId = 2, .role = UserRole::Owner, .text = "el gato está en el tejado"}).withheld);
  clock.seconds = 1'790'000'900;
  link->setKey(sealed(kPhrase));
  REQUIRE(eventually([&link] { return link->attempts().size() == 1; }));
  CHECK(link->attempts().front().saidAt == 1'790'000'200);
  screen.requestStop();
}

TEST_CASE("raises written down before a restart are read once the memory store opens, even after the worker started")
{
  const std::string db = scratchDb("outbox-late-store.db");
  const std::string key = sealed(kPhrase);
  {
    SqliteGraph graph;
    REQUIRE(graph.open(db));
    const auto unreachable = std::make_shared<ScriptedLink>(key);
    unreachable->setUnreachable(true);
    DuressScreen before({.link = unreachable, .eligible = disarmer, .outbox = outboxOver(graph), .clock = {}},
                        quick());
    before.start();
    REQUIRE(eventually([&unreachable, &before] { return unreachable->asked() > 0 && before.idle(); }));
    CHECK(before.screen({.userId = 3, .role = UserRole::Owner, .text = "el gato está en el tejado"}).withheld);
  }
  REQUIRE(outboxRows(db).size() == 1);

  SqliteGraph graph;
  const auto guard = std::make_shared<ScriptedLink>(key);
  DuressScreen after({.link = guard, .eligible = disarmer, .outbox = outboxOver(graph), .clock = {}}, quick());
  after.start();
  REQUIRE(eventually([&guard] { return guard->asked() > 0; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(guard->attempts().empty());
  CHECK_FALSE(after.idle());

  REQUIRE(graph.open(db));
  REQUIRE(eventually([&guard, &after, &db] { return guard->attempts().size() == 1 && after.idle() && outboxRows(db).empty(); }));
  CHECK(guard->attempts().front().userId == 3);
  CHECK(guard->attempts().front().key == key);
  after.requestStop();
}

TEST_CASE("the raise row commits through its own fully synced connection, waiting for the shared writer")
{
  const std::string db = scratchDb("outbox-durable.db");
  SqliteGraph graph;
  REQUIRE(graph.open(db));
  const auto outbox = std::make_shared<SqliteDuressOutbox>(
      SqliteDuressOutboxInput{.mutex = &graph.mutex(), .db = [&graph] { return graph.handle(); }});
  CHECK(pragmaOf(graph.handle(), "PRAGMA synchronous") == 1);
  CHECK(outbox->synchronous() == 2);
  CHECK(pragmaOf(graph.handle(), "PRAGMA synchronous") == 1);

  const int sharedChanges = sqlite3_total_changes(graph.handle());
  REQUIRE(sqlite3_exec(graph.handle(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
  std::atomic<bool> written{false};
  std::optional<int64_t> row;
  std::thread raise([&outbox, &row, &written] {
    row = outbox->add({.userId = 5, .role = UserRole::Resident, .keyDigest = "digest", .saidAt = 1'790'000'000});
    written = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_FALSE(written.load());
  REQUIRE(sqlite3_exec(graph.handle(), "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
  raise.join();
  REQUIRE(row.has_value());
  CHECK(sqlite3_total_changes(graph.handle()) == sharedChanges);
  const std::vector<OutboxRow> rows = outboxRows(db);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().userId == 5);
  CHECK(rows.front().saidAt == 1'790'000'000);

  outbox->remove(row.value_or(0));
  CHECK(outboxRows(db).empty());
  CHECK(sqlite3_total_changes(graph.handle()) == sharedChanges + 1);
}

TEST_CASE("a raise whose phrase changed while argus-llm was down is crossed out without asking guard")
{
  const std::string db = scratchDb("outbox-stale.db");
  {
    SqliteGraph graph;
    REQUIRE(graph.open(db));
    const auto unreachable = std::make_shared<ScriptedLink>(sealed(kPhrase));
    unreachable->setUnreachable(true);
    DuressScreen before({.link = unreachable, .eligible = disarmer, .outbox = outboxOver(graph)}, quick());
    before.start();
    REQUIRE(eventually([&unreachable, &before] { return unreachable->asked() > 0 && before.idle(); }));
    CHECK(before.screen({.userId = 3, .role = UserRole::Owner, .text = "el gato está en el tejado"}).withheld);
  }
  REQUIRE(outboxRows(db).size() == 1);

  SqliteGraph graph;
  REQUIRE(graph.open(db));
  const auto guard = std::make_shared<ScriptedLink>(sealed("la tía Rosa no viene"));
  DuressScreen after({.link = guard, .eligible = disarmer, .outbox = outboxOver(graph)}, quick());
  after.start();
  REQUIRE(eventually([&guard, &after, &db] { return guard->asked() > 0 && after.idle() && outboxRows(db).empty(); }));
  CHECK(guard->attempts().empty());
  after.requestStop();
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
