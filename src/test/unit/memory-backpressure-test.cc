#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/remote/wire-memory-chat.hxx>
#include <shared/services/sqlite/vec-db.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace
{

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "database/memory-schema.sql"
#endif
#ifndef ARGUS_TEST_MEMORY_MODELS_DIR
#define ARGUS_TEST_MEMORY_MODELS_DIR "models/memory"
#endif

constexpr const char* kScratchConfig = "memory-backpressure-test.toml";
constexpr const char* kScratchDir = "/tmp/f46-memory-backpressure";

// A controllable chat substrate: busy() is the gate the idle wait polls,
// chat() counts the jobs that actually reached the engine and can be held
// so the worker accumulates queue depth.
class FakeChat final : public IMemoryChat
{
public:
  bool available() const override { return true; }
  bool busy() const override { return busy_; }
  std::string chat(const ChatRequest&) const override
  {
    ++calls_;
    std::unique_lock lock(gateMutex_);
    // Self-release so a failed assertion can never deadlock the worker
    // join at teardown.
    gateCv_.wait_for(lock, std::chrono::seconds(5),
                     [this] { return released_; });
    return "resumen";
  }

  void setBusy(bool busy) { busy_ = busy; }
  void release()
  {
    {
      std::lock_guard lock(gateMutex_);
      released_ = true;
    }
    gateCv_.notify_all();
  }
  int calls() const { return calls_; }

private:
  std::atomic<bool> busy_{false};
  mutable std::atomic<int> calls_{0};
  mutable std::mutex gateMutex_;
  mutable std::condition_variable gateCv_;
  bool released_{false};
};

void writeConfig(const std::string& dbFile, int queueBound)
{
  std::remove(kScratchConfig);
  std::ofstream config(kScratchConfig);
  config << "[database]\nfile = \"" << kScratchDir << "/" << dbFile << "\"\n"
         << "[memory]\n"
         << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
         << "catalog_person_table = \"catalog_person\"\n"
         << "catalog_camera_table = \"catalog_camera\"\n"
         << "catalog_zone_table = \"catalog_zone\"\n"
         << "catalog_stream_table = \"catalog_stream\"\n"
         << "create_face_vec = false\n"
         << "queue_bound = " << queueBound << "\n"
         << "embedding_model = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
         << "/model.onnx\"\n"
         << "embedding_tokenizer = \"" << ARGUS_TEST_MEMORY_MODELS_DIR
         << "/tokenizer.json\"\n"
         << "embedding_preload = true\n"
         << "[extract]\n"
         << "model_path = \"/tmp/f46-memory-backpressure/none.gguf\"\n";
}

} // namespace

TEST_CASE("waitForIdle rides the chat port's busy gate (Ruling BZ)")
{
  std::filesystem::create_directories(kScratchDir);

  writeConfig("gate.db", 2);
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/gate.db");

  FakeChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());

  // waitForIdle blocks while the engine reports busy — the legacy isBusy
  // semantics through the port — and returns as soon as it is idle.
  chat.setBusy(true);
  const auto t0 = std::chrono::steady_clock::now();
  service.waitForIdle(400);
  CHECK(std::chrono::steady_clock::now() - t0 >=
        std::chrono::milliseconds(380));
  chat.setBusy(false);
  const auto t1 = std::chrono::steady_clock::now();
  service.waitForIdle(400);
  CHECK(std::chrono::steady_clock::now() - t1 <
        std::chrono::milliseconds(380));

  service.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("the bounded work queue drops compactions instead of growing "
          "(Ruling BZ)")
{
  std::filesystem::create_directories(kScratchDir);

  writeConfig("bound.db", 2);
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/bound.db");

  FakeChat chat;
  MemoryService service(VecDb::instance(), chat);
  service.init({});
  REQUIRE(service.isLoaded());

  // The first compaction reaches the engine and is held there, so the
  // queue grows behind it and the bound bites deterministically.
  service.enqueueCompaction(
      1, "user: mi hermana se llama Ana\nassistant: anotado", "es");
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (chat.calls() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(chat.calls() == 1);

  // Four more arrive behind the held one: at queue_bound 2 the last two
  // drop instead of growing the queue.
  for (int i = 0; i < 4; ++i) {
    service.enqueueCompaction(
        1, "user: mi hermana se llama Ana\nassistant: anotado", "es");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(chat.calls() == 1);

  chat.release();
  CHECK(service.flushPending(10000));
  // The held one plus the two retained behind it; the other two dropped.
  CHECK(chat.calls() == 3);

  service.shutdown();
  std::remove(kScratchConfig);
}

TEST_CASE("the wire chat port never reports busy and never blocks the idle "
          "wait")
{
  // WireMemoryChat never polls the remote engine: back-pressure is the
  // queue-depth gate, so busy() is always false and waitForIdle returns
  // immediately even with the remote unreachable (Ruling BZ).
  WireMemoryChat wire("127.0.0.1:1", 50);
  CHECK(wire.available());
  CHECK_FALSE(wire.busy());

  writeConfig("port.db", 64);
  ConfigService::load(kScratchConfig);
  std::filesystem::remove(std::string(kScratchDir) + "/port.db");

  MemoryService service(VecDb::instance(), wire);
  service.init({});
  REQUIRE(service.isLoaded());
  const auto t0 = std::chrono::steady_clock::now();
  service.waitForIdle(10000);
  CHECK(std::chrono::steady_clock::now() - t0 <
        std::chrono::milliseconds(380));

  service.shutdown();
  std::remove(kScratchConfig);
}
