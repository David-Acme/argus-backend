#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/vocabulary/tool-contracts.hxx>
#include <config/config-service.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <sqlite/vec-db.hxx>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "services/llm/database/schema.sql"
#endif
#ifndef ARGUS_TEST_MEMORY_MODELS_DIR
#define ARGUS_TEST_MEMORY_MODELS_DIR "models/memory"
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
  std::string chat(const ChatRequest&) const override { return {}; }
};

void writeConfig(const std::string& database = "reminder.db")
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
         << "model_path = \"" << kScratchDir << "/none.gguf\"\n";
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
      if (descriptor.name == name)
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
      if (descriptor.name == call.name)
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
  const auto strangerForgot = run(stranger);
  CHECK_FALSE(strangerForgot.ok);
  auto dog = callFor("memory.recall", kSpeaker);
  dog.arguments["query"] = "perro";
  CHECK(lowered(run(dog).output).find("toby") != std::string::npos);

  auto forget = callFor("memory.forget", kSpeaker);
  forget.arguments["query"] = "mi perro Toby";
  const auto forgot = run(forget);
  INFO("forget output: " << forgot.output);
  REQUIRE(forgot.ok);
  CHECK(forgot.output.starts_with("Olvidado: "));
  CHECK(lowered(run(dog).output).find("toby") == std::string::npos);
  CHECK_FALSE(run(forget).ok);

  auto nothing = callFor("memory.forget", kSpeaker);
  nothing.arguments["query"] = "el color favorito de la vecina";
  CHECK_FALSE(run(nothing).ok);

  service.shutdown();
  std::remove(kScratchConfig);
}
