#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/vocabulary/tool-contracts.hxx>
#include <config/config-service.hxx>
#include <feature/memory/services/memory/memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <sqlite/vec-db.hxx>

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

void writeConfig()
{
  std::remove(kScratchConfig);
  std::ofstream config(kScratchConfig);
  config << "[database]\nfile = \"" << kScratchDir << "/reminder.db\"\n"
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
