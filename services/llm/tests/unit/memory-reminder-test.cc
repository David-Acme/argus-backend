#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <llama.h>
#include <shared/vocabulary/tool-contracts.hxx>
#include <sqlite3.h>
#include <config/config-service.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <feature/memory/services/memory/reminder-call-scheduler.hxx>
#include <feature/memory/services/memory/reminder-row-writer.hxx>
#include <sqlite/vec-db.hxx>

#include <algorithm>
#include <array>
#include <ctime>
#include <memory>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif
#ifndef ARGUS_TEST_MEMORY_MODELS_DIR
#define ARGUS_TEST_MEMORY_MODELS_DIR "models/memory"
#endif
#ifndef ARGUS_TEST_EXTRACT_MODEL
#define ARGUS_TEST_EXTRACT_MODEL "models/extract/NuExtract-1.5-tiny-Q4_K_M.gguf"
#endif

constexpr const char* kScratchConfig = "memory-reminder-test.toml";
constexpr const char* kScratchDir = "/tmp/f9-memory-reminder";
constexpr int64_t kSpeaker = 7;
constexpr int64_t kOtherUser = 9;

class SilentChat final : public IMemoryChat
{
public:
  bool available() const override { return false; }
  bool busy() const override { return false; }
  [[nodiscard]] std::string chat(const ChatRequest&) const override { return {}; }
};

void writeConfig(const std::string& database = "reminder.db",
                 const std::string& extractModel = std::string(kScratchDir) + "/none.gguf")
{
  std::remove(kScratchConfig);
  std::ofstream config(kScratchConfig);
  config << "[database]\nfile = \"" << kScratchDir << "/" << database << "\"\n"
         << "[memory]\n"
         << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
         << "catalog_person_table = \"catalog_person\"\n"
         << "catalog_camera_table = \"catalog_camera\"\n"
         << "catalog_zone_table = \"catalog_zone\"\n"
         << "catalog_stream_table = \"catalog_stream\"\n"
         << "create_face_vec = false\n"
         << "embedding_model = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
         << "/model.onnx\"\n"
         << "embedding_tokenizer = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
         << "/tokenizer.json\"\n"
         << "embedding_preload = true\n"
         << "[extract]\n"
         << "model_path = \"" << extractModel << "\"\n";
}

std::string weekdayName(int weekday)
{
  constexpr std::array<const char*, 7> kDays{"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"};
  return kDays.at(static_cast<std::size_t>(weekday));
}

std::string lowered(std::string text)
{
  for (auto& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

tools::ToolCall callFor(const std::string& name, int64_t userId)
{
  tools::ToolCall call;
  call.name = name;
  call.arguments = Json::Value(Json::objectValue);
  call.context.userId = userId;
  call.context.lang = "es";
  call.context.channel = "tool_result";
  return call;
}

}

TEST_CASE("a reminder is written for the speaking user and no one else")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig();
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/reminder.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());

  const auto descriptors = service.toolDescriptors();
  const auto tool = [&descriptors](const std::string& name) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == name)
        return &descriptor;
    return static_cast<const tools::ToolDescriptor*>(nullptr);
  };
  REQUIRE(tool("memory.remind") != nullptr);
  REQUIRE(tool("memory.recall") != nullptr);

  auto remind = callFor("memory.remind", kSpeaker);
  remind.arguments["text"] = "recuerdame que mi cita con el dentista es el lunes";
  remind.context.utterance = remind.arguments["text"].asString();
  const auto stored = tool("memory.remind")->handler(remind);
  INFO("remind output: " << stored.output);
  REQUIRE(stored.ok);

  auto mine = callFor("memory.recall", kSpeaker);
  mine.arguments["query"] = "dentista";
  const auto ownRecall = tool("memory.recall")->handler(mine);
  CHECK(ownRecall.ok);
  CHECK(ownRecall.output.find("dentista") != std::string::npos);

  auto theirs = callFor("memory.recall", kOtherUser);
  theirs.arguments["query"] = "dentista";
  const auto otherRecall = tool("memory.recall")->handler(theirs);
  CHECK(otherRecall.output.find("dentista") == std::string::npos);

  auto routed = callFor("memory.remind", kSpeaker);
  routed.arguments["text"] = "mi revision del coche cae el jueves";
  routed.context.utterance = routed.arguments["text"].asString();
  routed.context.decided = true;
  const auto routedStored = tool("memory.remind")->handler(routed);
  INFO("routed output: " << routedStored.output);
  CHECK(routedStored.ok);

  service.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("memory writes keep to the user's own words and forgetting stays in scope")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("grounding.db");
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/grounding.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());

  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };

  auto fromNote = callFor("memory.remember", kSpeaker);
  fromNote.arguments["text"] = "Una persona se acerca a la puerta del garaje";
  fromNote.context.utterance = "recuérdalo";
  const auto noteRefused = run(fromNote);
  INFO("note output: " << noteRefused.output);
  CHECK_FALSE(noteRefused.ok);
  auto garage = callFor("memory.recall", kSpeaker);
  garage.arguments["query"] = "garaje";
  CHECK(run(garage).output.find("garaje") == std::string::npos);

  auto paraphrased = callFor("memory.remember", kSpeaker);
  paraphrased.arguments["text"] = "El usuario tiene una mascota de nombre Toby";
  paraphrased.arguments["subject"] = "usuario";
  paraphrased.arguments["predicate"] = "mascota";
  paraphrased.arguments["value"] = "una mascota de nombre Toby";
  paraphrased.context.utterance = "recuerda que mi perro se llama Toby";
  paraphrased.context.decided = true;
  const auto saved = run(paraphrased);
  INFO("saved output: " << saved.output);
  REQUIRE(saved.ok);
  CHECK(lowered(saved.output).find("perro") != std::string::npos);
  CHECK(lowered(saved.output).find("mascota") == std::string::npos);
  CHECK(saved.output.find("(id") == std::string::npos);

  auto english = callFor("memory.remember", kSpeaker);
  english.context.lang = "en";
  english.context.utterance = "remember that my sister comes on sundays";
  const auto savedEnglish = run(english);
  INFO("english output: " << savedEnglish.output);
  if (savedEnglish.ok)
    CHECK(savedEnglish.output.starts_with("Saved: "));

  auto stranger = callFor("memory.forget", kOtherUser);
  stranger.arguments["query"] = "perro Toby";
  stranger.context.utterance = "olvida lo de mi perro Toby";
  const auto strangerForgot = run(stranger);
  CHECK_FALSE(strangerForgot.ok);
  auto dog = callFor("memory.recall", kSpeaker);
  dog.arguments["query"] = "perro";
  CHECK(lowered(run(dog).output).find("toby") != std::string::npos);

  auto injected = callFor("memory.forget", kSpeaker);
  injected.arguments["query"] = "mi perro Toby";
  injected.context.utterance = "qué tiempo hace hoy";
  CHECK_FALSE(run(injected).ok);
  auto unspoken = callFor("memory.forget", kSpeaker);
  unspoken.arguments["query"] = "mi perro Toby";
  CHECK_FALSE(run(unspoken).ok);
  CHECK(lowered(run(dog).output).find("toby") != std::string::npos);

  auto forget = callFor("memory.forget", kSpeaker);
  forget.arguments["query"] = "mi perro Toby";
  forget.context.utterance = "olvida lo de mi perro Toby";
  const auto forgot = run(forget);
  INFO("forget output: " << forgot.output);
  REQUIRE(forgot.ok);
  CHECK(forgot.output.starts_with("Olvidado: "));
  const std::string afterForget = run(dog).output;
  INFO("recall after forget: " << afterForget);
  CHECK(lowered(afterForget).find("toby") == std::string::npos);
  CHECK_FALSE(run(forget).ok);

  auto timed = callFor("memory.remind", kSpeaker);
  timed.context.utterance = "Recuérdame mañana a las nueve llamar al dentista.";
  timed.arguments["text"] = timed.context.utterance;
  timed.context.decided = true;
  const auto timedSaved = run(timed);
  INFO("timed output: " << timedSaved.output);
  REQUIRE(timedSaved.ok);
  CHECK(lowered(timedSaved.output).find("dentista") != std::string::npos);
  CHECK(lowered(timedSaved.output).find("nueve") != std::string::npos);
  CHECK(timedSaved.output.find("..") == std::string::npos);

  auto note = callFor("memory.remember", kSpeaker);
  note.context.utterance = "anota que llegó el paquete";
  note.arguments["text"] = note.context.utterance;
  note.context.decided = true;
  const auto noted = run(note);
  INFO("note output: " << noted.output);
  REQUIRE(noted.ok);
  CHECK(lowered(noted.output).find("paquete") != std::string::npos);

  auto camera = callFor("memory.remind", kSpeaker);
  camera.context.utterance = "muéstrame la cámara 3";
  camera.arguments["text"] = camera.context.utterance;
  camera.context.decided = true;
  CHECK_FALSE(run(camera).ok);

  auto command = callFor("memory.remember", kSpeaker);
  command.context.utterance = "enciende la luz de la cocina";
  command.arguments["text"] = command.context.utterance;
  command.context.decided = true;
  CHECK_FALSE(run(command).ok);

  auto nothing = callFor("memory.forget", kSpeaker);
  nothing.arguments["query"] = "el color favorito de la vecina";
  nothing.context.utterance = "olvida lo del color favorito de la vecina";
  CHECK_FALSE(run(nothing).ok);

  service.shutdown();
  std::remove(kScratchConfig);
}

namespace
{
struct OpenFact
{
  std::string predicate;
  std::string canonical;
};

std::vector<OpenFact> openFacts(const std::string& database)
{
  std::vector<OpenFact> facts;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(database.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return facts;
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT predicate, canonical FROM memory_fact WHERE valid_to = 0", -1, &stmt,
                         nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW)
      facts.push_back({.predicate = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                       .canonical = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))});
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return facts;
}
}

TEST_CASE("an explicit request kept as a note is refined off the turn by the extraction model")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("refine.db", ARGUS_TEST_EXTRACT_MODEL);
  ConfigService::load(kScratchConfig);
  const std::string database = std::string(kScratchDir) + "/refine.db";
  std::filesystem::remove(database);
  llama_backend_init();
  {
    SilentChat chat;
    MemoryService service(VecDb::instance(), chat);
    service.init({});
    REQUIRE(service.isLoaded());
    const auto descriptors = service.toolDescriptors();
    const auto run = [&descriptors](const tools::ToolCall& call) {
      for (const auto& descriptor : descriptors)
        if (descriptor.spec.name == call.name)
          return descriptor.handler(call);
      return tools::ToolResult{};
    };

    auto gate = callFor("memory.remember", kSpeaker);
    gate.context.utterance = "guarda que el código del portón es 1234";
    gate.arguments["text"] = gate.context.utterance;
    gate.context.decided = true;
    const auto saved = run(gate);
    INFO("gate output: " << saved.output);
    REQUIRE(saved.ok);
    CHECK(saved.output.find("1234") != std::string::npos);

    auto parcel = callFor("memory.remember", kSpeaker);
    parcel.context.utterance = "anota que llegó el paquete";
    parcel.arguments["text"] = parcel.context.utterance;
    parcel.context.decided = true;
    REQUIRE(run(parcel).ok);

    REQUIRE(service.flushPending(180000));
    const auto facts = openFacts(database);
    const auto holding = [&facts](const std::string& needle) {
      return std::ranges::count_if(facts, [&needle](const OpenFact& fact) {
        return fact.canonical.find(needle) != std::string::npos;
      });
    };
    for (const auto& fact : facts)
      MESSAGE("open fact: " << fact.predicate << " | " << fact.canonical);
    CHECK(holding("1234") == 1);
    CHECK(holding("paquete") == 1);
    const bool gateRefined = std::ranges::any_of(facts, [](const OpenFact& fact) {
      return fact.canonical.find("1234") != std::string::npos && fact.predicate != "nota";
    });
    CHECK(gateRefined);

    auto recall = callFor("memory.recall", kSpeaker);
    recall.arguments["query"] = "código del portón";
    CHECK(run(recall).output.find("1234") != std::string::npos);
    service.shutdown();
  }
  llama_backend_free();
  std::remove(kScratchConfig);
}

TEST_CASE("a newer value of the same fact closes the older one, and notes never close each other")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("supersede.db");
  ConfigService::load(kScratchConfig);
  const std::string database = std::string(kScratchDir) + "/supersede.db";
  std::filesystem::remove(database);
  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };
  const auto remember = [&run](const std::string& utterance) {
    auto call = callFor("memory.remember", kSpeaker);
    call.context.utterance = utterance;
    call.arguments["text"] = utterance;
    call.context.decided = true;
    return run(call);
  };

  REQUIRE(remember("recuerda que mi hermana viene los domingos").ok);
  REQUIRE(remember("recuerda que mi hermana viene los sábados").ok);
  REQUIRE(remember("anota que llegó el paquete").ok);
  REQUIRE(remember("anota que el wifi se cae cada semana").ok);
  REQUIRE(remember("anota que llegó el paquete").ok);
  REQUIRE(service.flushPending(60000));

  const auto facts = openFacts(database);
  for (const auto& fact : facts)
    MESSAGE("open fact: " << fact.predicate << " | " << fact.canonical);
  const auto holding = [&facts](const std::string& needle) {
    return std::ranges::count_if(facts, [&needle](const OpenFact& fact) {
      return fact.canonical.find(needle) != std::string::npos;
    });
  };
  CHECK(holding("domingos") == 0);
  CHECK(holding("sábados") == 1);
  CHECK(holding("paquete") == 1);
  CHECK(holding("wifi") == 1);
  service.shutdown();
  std::remove(kScratchConfig);
}

namespace
{
class RecordingReminderCalls final : public ReminderCallScheduler
{
public:
  [[nodiscard]] bool schedule(const ReminderCallRequest& request) const override
  {
    requests.push_back(request);
    return true;
  }

  mutable std::vector<ReminderCallRequest> requests;
};
}

TEST_CASE("a reminder that names a time schedules a call at that time")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("reminder-call.db");
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/reminder-call.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  auto calls = std::make_shared<RecordingReminderCalls>();
  service.setReminderCalls(calls);

  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };

  auto timed = callFor("memory.remind", kSpeaker);
  timed.arguments["text"] = "llamar al dentista mañana a las nueve";
  timed.context.utterance = "recuérdame mañana a las nueve llamar al dentista";
  timed.context.decided = true;
  const auto stored = run(timed);
  INFO("timed output: " << stored.output);
  REQUIRE(stored.ok);
  REQUIRE(calls->requests.size() == 1);
  const auto& request = calls->requests.front();
  CHECK(request.userId == kSpeaker);
  CHECK(request.topic == "llamar al dentista");
  CHECK(request.lang == "es");
  CHECK(request.commandId.starts_with("memory-remind:"));
  const auto seconds = static_cast<std::time_t>(request.fireAt);
  std::tm local{};
  localtime_r(&seconds, &local);
  CHECK(local.tm_hour == 9);
  CHECK(local.tm_min == 0);
  CHECK(request.fireAt > std::time(nullptr));
  CHECK(stored.output.find(std::string("Te llamaré el ") + weekdayName(local.tm_wday) + " " + std::to_string(local.tm_mday)) != std::string::npos);
  CHECK(stored.output.ends_with(" a las 9 de la mañana."));

  auto untimed = callFor("memory.remind", kSpeaker);
  untimed.arguments["text"] = "mi cita con el dentista es el lunes";
  untimed.context.utterance = "recuérdame que mi cita con el dentista es el lunes";
  const auto plain = run(untimed);
  INFO("untimed output: " << plain.output);
  CHECK(calls->requests.size() == 1);
  CHECK(plain.output.find("llamaré") == std::string::npos);

  service.shutdown();
  std::remove(kScratchConfig);
}

namespace
{
class RecordingRows final : public ReminderRowWriter
{
public:
  [[nodiscard]] bool create(const ReminderRowRequest& request) const override
  {
    created.push_back(request);
    return accepts;
  }

  [[nodiscard]] std::optional<std::vector<ReminderRowInfo>> list(const ReminderRowQuery& query) const override
  {
    queries.push_back(query);
    if (!reachable)
      return std::nullopt;
    std::vector<ReminderRowInfo> rows;
    for (const auto& [owner, row] : stored)
      if (owner == query.userId && (query.includeCompleted || !row.completed))
        rows.push_back(row);
    return rows;
  }

  bool accepts{true};
  bool reachable{true};
  std::vector<std::pair<int64_t, ReminderRowInfo>> stored;
  mutable std::vector<ReminderRowRequest> created;
  mutable std::vector<ReminderRowQuery> queries;
};

class RecordingCalls final : public ReminderCallScheduler
{
public:
  [[nodiscard]] bool schedule(const ReminderCallRequest& request) const override
  {
    scheduled.push_back(request);
    return true;
  }

  mutable std::vector<ReminderCallRequest> scheduled;
};

tools::ToolCall remindCall(int64_t userId, const std::string& utterance)
{
  auto call = callFor("memory.remind", userId);
  call.context.role = UserRole::Resident;
  call.context.utterance = utterance;
  call.arguments["text"] = utterance;
  call.context.decided = true;
  return call;
}
}

TEST_CASE("a timed reminder is written once, for the speaker, through the reminder rows and never for another user")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("rows.db");
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/rows.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  const auto rows = std::make_shared<RecordingRows>();
  const auto calls = std::make_shared<RecordingCalls>();
  service.setReminderRows(rows);
  service.setReminderCalls(calls);

  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };

  auto timed = remindCall(kSpeaker, "Recuérdame mañana a las nueve llamar al dentista.");
  timed.arguments["userId"] = static_cast<Json::Int64>(kOtherUser);
  timed.arguments["user_id"] = static_cast<Json::Int64>(kOtherUser);
  timed.arguments["targetUserId"] = static_cast<Json::Int64>(kOtherUser);
  timed.arguments["target_user_id"] = static_cast<Json::Int64>(kOtherUser);
  timed.arguments["user"] = "9";
  const auto saved = run(timed);
  INFO("timed output: " << saved.output);
  REQUIRE(saved.ok);
  REQUIRE(rows->created.size() == 1);
  CHECK(rows->created.front().userId == kSpeaker);
  CHECK(rows->created.front().role == "resident");
  CHECK(rows->created.front().title.find("dentista") != std::string::npos);
  CHECK(rows->created.front().title.find("mañana") == std::string::npos);
  CHECK(rows->created.front().scheduledAt > static_cast<int64_t>(std::time(nullptr)));
  CHECK(rows->created.front().commandId.starts_with("memory-remind:"));
  REQUIRE(calls->scheduled.size() == 1);
  CHECK(calls->scheduled.front().userId == kSpeaker);
  CHECK(calls->scheduled.front().commandId == rows->created.front().commandId);
  CHECK(saved.output.find("Te llamaré el ") != std::string::npos);
  CHECK(saved.output.ends_with(" a las 9 de la mañana."));

  auto another = remindCall(kOtherUser, "Recuérdame mañana a las diez comprar pan.");
  another.arguments["userId"] = static_cast<Json::Int64>(kSpeaker);
  REQUIRE(run(another).ok);
  REQUIRE(rows->created.size() == 2);
  CHECK(rows->created.back().userId == kOtherUser);
  CHECK(std::ranges::none_of(rows->created, [](const ReminderRowRequest& row) { return row.userId != kSpeaker && row.userId != kOtherUser; }));

  const std::time_t nowSeconds = std::time(nullptr);
  std::tm tomorrow{};
  localtime_r(&nowSeconds, &tomorrow);
  tomorrow.tm_mday += 1;
  tomorrow.tm_isdst = -1;
  std::mktime(&tomorrow);
  const std::string disagreeing = "Recuérdame el " + weekdayName((tomorrow.tm_wday + 2) % 7) + " " + std::to_string(tomorrow.tm_mday) +
                                  " a las 5 llamar a Juan.";
  auto disputed = remindCall(kSpeaker, disagreeing);
  const size_t calledBefore = calls->scheduled.size();
  REQUIRE(run(disputed).ok);
  CHECK(calls->scheduled.size() == calledBefore);
  const int64_t heard = static_cast<int64_t>(nowSeconds) + 7200;
  disputed.context.heardAt = heard;
  REQUIRE(run(disputed).ok);
  REQUIRE(calls->scheduled.size() == calledBefore + 1);
  CHECK(calls->scheduled.back().fireAt == heard);
  CHECK(calls->scheduled.back().topic.find("Juan") != std::string::npos);
  CHECK(calls->scheduled.back().topic.find(weekdayName((tomorrow.tm_wday + 2) % 7)) == std::string::npos);

  const size_t before = rows->created.size();
  const auto timeless = run(remindCall(kSpeaker, "Recuérdame que mi cita con el dentista es el lunes."));
  CHECK(timeless.ok);
  CHECK(rows->created.size() == before);

  service.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("a reminder that no row writer accepts is not claimed, and rows alone are spoken as the reminders list")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("rows-only.db");
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/rows-only.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  const auto rows = std::make_shared<RecordingRows>();
  service.setReminderRows(rows);

  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };

  const auto listed = run(remindCall(kSpeaker, "Recuérdame mañana a las nueve llamar al dentista."));
  REQUIRE(listed.ok);
  CHECK(listed.output.find("Quedó en tus recordatorios para el ") != std::string::npos);
  CHECK(listed.output.ends_with(" a las 9 de la mañana."));
  CHECK(listed.output.find("Te llamaré") == std::string::npos);

  rows->accepts = false;
  const auto refused = run(remindCall(kSpeaker, "Recuérdame mañana a las once pagar la luz."));
  REQUIRE(refused.ok);
  CHECK(refused.output.find("recordatorios para el ") == std::string::npos);
  CHECK(refused.output.find("Te llamaré") == std::string::npos);

  service.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("the reminders list is the caller's own and says so when it cannot be read")
{
  std::filesystem::create_directories(kScratchDir);
  writeConfig("rows-list.db");
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/rows-list.db");

  SilentChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());
  const auto rows = std::make_shared<RecordingRows>();
  rows->stored = {{kSpeaker, {.id = 1, .title = "Llamar al dentista", .scheduledAt = 1900000000, .completed = false}},
                  {kSpeaker, {.id = 2, .title = "Pagar la luz", .scheduledAt = 1900003600, .completed = true}},
                  {kOtherUser, {.id = 3, .title = "Secreto de otro", .scheduledAt = 1900000000, .completed = false}}};

  const auto descriptors = service.toolDescriptors();
  const auto run = [&descriptors](const tools::ToolCall& call) {
    for (const auto& descriptor : descriptors)
      if (descriptor.spec.name == call.name)
        return descriptor.handler(call);
    return tools::ToolResult{};
  };

  auto list = callFor("reminder.list", kSpeaker);
  list.context.role = UserRole::Guest;

  const auto unreachable = run(list);
  CHECK_FALSE(unreachable.ok);
  CHECK(unreachable.output == "Ahora no puedo leer tus recordatorios.");

  service.setReminderRows(rows);
  const auto mine = run(list);
  REQUIRE(mine.ok);
  CHECK(mine.output.find("Llamar al dentista") != std::string::npos);
  CHECK(mine.output.find("Pagar la luz") == std::string::npos);
  CHECK(mine.output.find("Secreto de otro") == std::string::npos);
  CHECK(mine.data["reminders"].size() == 1);
  REQUIRE_FALSE(rows->queries.empty());
  CHECK(rows->queries.back().userId == kSpeaker);
  CHECK(rows->queries.back().role == "guest");

  list.arguments["include_done"] = true;
  list.arguments["user_id"] = static_cast<Json::Int64>(kOtherUser);
  const auto all = run(list);
  CHECK(all.data["reminders"].size() == 2);
  CHECK(all.output.find("Secreto de otro") == std::string::npos);
  CHECK(rows->queries.back().userId == kSpeaker);

  auto others = callFor("reminder.list", kOtherUser);
  others.context.role = UserRole::Guard;
  CHECK(run(others).output.find("Secreto de otro") != std::string::npos);
  CHECK(run(others).output.find("Llamar al dentista") == std::string::npos);

  auto english = callFor("reminder.list", 11);
  english.context.lang = "en";
  CHECK(run(english).output == "You have no pending reminders.");

  auto nobody = callFor("reminder.list", 0);
  CHECK_FALSE(run(nobody).ok);

  service.shutdown();
  std::remove(kScratchConfig);
}
