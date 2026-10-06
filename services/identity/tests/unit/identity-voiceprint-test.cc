#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-voiceprint-rpc-service.hxx>
#include <bit>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/voiceprint/repositories/voice-profile/voice-profile-repository.hxx>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <feature/voiceprint/services/passive/passive-enrollment-service.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <filesystem>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <identity/voiceprint-client.hxx>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <sync/identity-change-sink.hxx>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif
#ifndef ARGUS_TEST_SPEAKER_MODEL
#error "ARGUS_TEST_SPEAKER_MODEL must point at the speaker model"
#endif
#ifndef ARGUS_TEST_VOICE_FIXTURES
#error "ARGUS_TEST_VOICE_FIXTURES must point at tests/fixtures/voiceprint"
#endif

namespace
{

constexpr const char* kDb = "identity-voiceprint-test.db";
constexpr const char* kFleetSecret = "voiceprint-test-secret";
constexpr int64_t kOwner = 1;
constexpr int64_t kRita = 7;
constexpr int64_t kGil = 8;
constexpr int64_t kGus = 9;
constexpr int kRate = 16000;

using test_support::AppRunner;

bool waitForBoot()
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

class RecordingSink : public IdentityChangeSink
{
public:
  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    const std::scoped_lock lock(mutex_);
    actions_.push_back(input.event);
    transactional_ = transactional_ && input.client != nullptr;
    co_return;
  }

  [[nodiscard]] std::vector<UserActionEvent> actions() const
  {
    const std::scoped_lock lock(mutex_);
    return actions_;
  }

  [[nodiscard]] bool transactional() const
  {
    const std::scoped_lock lock(mutex_);
    return transactional_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<UserActionEvent> actions_;
  mutable bool transactional_{true};
};

std::vector<int16_t> fixture(const std::string& name)
{
  const std::filesystem::path path =
      std::filesystem::path(ARGUS_TEST_VOICE_FIXTURES) / (name + ".bin");
  std::ifstream file(path, std::ios::binary);
  const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};
  std::vector<int16_t> samples;
  samples.reserve(bytes.size() / 2);
  for (size_t index = 0; index + 1 < bytes.size(); index += 2)
    samples.push_back(std::bit_cast<int16_t>(
        static_cast<uint16_t>(static_cast<uint8_t>(bytes[index]) |
                              (static_cast<uint8_t>(bytes[index + 1]) << 8U))));
  return samples;
}

struct TurnSpec
{
  std::string clip;
  float offsetSeconds{0.0F};
  float seconds{2.6F};
  uint32_t seed{1};
  int repeats{1};
};

std::vector<int16_t> turnSamples(const TurnSpec& spec)
{
  const auto source = fixture(spec.clip);
  const auto begin = std::min(
      source.size(), static_cast<size_t>(spec.offsetSeconds * kRate));
  const auto end = std::min(
      source.size(), begin + static_cast<size_t>(spec.seconds * kRate));
  std::mt19937 rng(spec.seed);
  std::normal_distribution<float> noise(0.0F, 25.0F);
  std::uniform_real_distribution<float> gain(0.8F, 1.1F);
  const float level = gain(rng);
  std::vector<int16_t> out;
  out.reserve((end - begin) * static_cast<size_t>(spec.repeats));
  for (int repeat = 0; repeat < spec.repeats; ++repeat)
    for (size_t index = begin; index < end; ++index)
      out.push_back(static_cast<int16_t>(std::clamp(
          static_cast<float>(source[index]) * level + noise(rng), -32767.0F,
          32767.0F)));
  return out;
}

EncodedVoice pcmOf(const std::vector<int16_t>& samples)
{
  std::string bytes;
  bytes.reserve(samples.size() * 2);
  for (const int16_t sample : samples) {
    const auto word = std::bit_cast<uint16_t>(sample);
    bytes.push_back(static_cast<char>(word & 0xFFU));
    bytes.push_back(static_cast<char>((word >> 8U) & 0xFFU));
  }
  return {.bytes = std::move(bytes),
          .encoding = VoiceEncoding::Pcm16,
          .sampleRate = kRate};
}

std::vector<TurnSpec> callOf(const char* speaker, uint32_t seed)
{
  std::vector<TurnSpec> turns;
  const std::string prefix(speaker);
  const std::vector<std::string> clips{"-1", "-2", "-3", "-4"};
  turns.reserve(clips.size());
  for (size_t index = 0; index < clips.size(); ++index) {
    turns.push_back({.clip = prefix + clips[(index + seed) % clips.size()],
                     .offsetSeconds = 0.05F * static_cast<float>((seed + index) % 4),
                     .seconds = 3.0F,
                     .seed = seed * 31 + static_cast<uint32_t>(index),
                     .repeats = 1});
  }
  return turns;
}

void seedUsers()
{
  auto client = DbService::identityClient();
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active) VALUES "
      "(1, 'Ada', 'Owner', 'owner', 'es', 1), "
      "(7, 'Rita', 'Resident', 'resident', 'en', 1), "
      "(8, 'Gil', 'Guard', 'guard', 'es', 1), "
      "(9, 'Gus', 'Guest', 'guest', 'es', 1), "
      "(10, 'Vera', 'Inactive', 'guest', 'es', 0)");
  client->execSqlSync(
      "INSERT INTO person (id, user_id, name) VALUES (70, 7, 'Rita')");
  client->execSqlSync(
      "INSERT INTO user_privacy (user_id, notice_version, presence, "
      "face_cameras, voice_learning, camera_audio) "
      "SELECT id, 1, 1, 1, 1, 1 FROM user WHERE id IN (1, 7, 8, 9)");
}

struct Teardown
{
  Teardown() = default;
  ~Teardown()
  {
    identity_change::setSink(nullptr);
    SpeakerEmbeddingService::instance().shutdown();
  }

  Teardown(const Teardown&) = delete;
  Teardown& operator=(const Teardown&) = delete;
};

IdentityVoiceprintConfig testConfig()
{
  IdentityVoiceprintConfig config;
  config.modelPath = ARGUS_TEST_SPEAKER_MODEL;
  return config;
}

struct LocalMoment
{
  int day;
  int hour;
};

int64_t localTime(LocalMoment at)
{
  std::tm moment{};
  moment.tm_year = 2026 - 1900;
  moment.tm_mon = 8;
  moment.tm_mday = 1 + at.day;
  moment.tm_hour = at.hour;
  moment.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&moment));
}

struct CallInput
{
  int64_t userId{0};
  std::string device;
  std::string key;
  std::vector<TurnSpec> turns;
  int64_t at{0};
};

struct CallResult
{
  std::vector<VoiceTurnLearning> turns;
  PassiveCallOutcome outcome{PassiveCallOutcome::NotFound};
};

CallResult runCall(const PassiveEnrollmentService& passive,
                   const CallInput& input)
{
  CallResult result;
  int64_t now = input.at;
  for (const auto& spec : input.turns) {
    result.turns.push_back(drogon::sync_wait(
        passive.learnFromTurn({.userId = input.userId,
                               .deviceHash = input.device,
                               .callKey = input.key,
                               .sample = pcmOf(turnSamples(spec)),
                               .now = now})));
    now += 20;
  }
  result.outcome = drogon::sync_wait(
      passive.closeCall({.callKey = input.key, .now = now}));
  return result;
}

int64_t count(const std::string& sql)
{
  const auto rows = DbService::identityClient()->execSqlSync(sql);
  return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

std::vector<std::string> events(const RecordingSink& sink)
{
  std::vector<std::string> names;
  for (const auto& action : sink.actions())
    names.push_back(action.newData.get("event", "").asString());
  return names;
}

struct Fleet
{
  IdentityVoiceprintRpcService service;
  std::unique_ptr<grpc::Server> server;
  std::string target;

  explicit Fleet(std::vector<std::pair<std::string, std::string>> callers = {})
      : service({.gate = std::make_shared<const argus::client::FleetCallerGate>(
                     argus::client::FleetGateConfig{
                         .expectedCallers = identity_callers::expected(),
                         .callerPairs = std::move(callers),
                         .legacySecret = kFleetSecret,
                         .onFirstLegacy = {}}),
                 .voiceprint = testConfig()})
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    target = "127.0.0.1:" + std::to_string(port);
  }

  ~Fleet()
  {
    if (server)
      server->Shutdown();
  }

  Fleet(const Fleet&) = delete;
  Fleet& operator=(const Fleet&) = delete;
};

}

TEST_CASE("a voice is learned from its owner's own calls, and only from them" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  for (const char* suffix : {"", "-wal", "-shm"})
    std::remove((std::string(kDb) + suffix).c_str());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot());
  DbService::installExtensions();
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  seedUsers();
  VecDb::instance().setDbFile(kDb);
  FaceService::instance().disable();

  RecordingSink sink;
  identity_change::setSink(&sink);
  const Teardown teardown;

  {
    INFO("an explicit enrollment from before is carried over once");
    auto client = DbService::identityClient();
    client->execSqlSync(
        "CREATE TABLE voiceprint (id INTEGER PRIMARY KEY, user_id INTEGER, "
        "model TEXT, embedding BLOB, sample_count INTEGER, speech_seconds "
        "REAL, method TEXT, consent_version TEXT, enrolled_by INTEGER, "
        "created_at INTEGER)");
    client->execSqlSync("CREATE TABLE voiceprint_challenge (id INTEGER)");
    client->execSqlSync("CREATE TABLE voiceprint_challenge_sample (id INTEGER)");
    client->execSqlSync(
        "INSERT INTO voiceprint VALUES (1, 9, 'legacy-model', x'0000803f', 3, "
        "9.5, 'self', 'voiceprint-consent-v1', 9, 1700000000)");
    VoiceProfileRepository::migrateLegacy();
    VoiceProfileRepository::migrateLegacy();
    CHECK(count("SELECT COUNT(*) AS total FROM sqlite_master WHERE name IN "
                "('voiceprint', 'voiceprint_challenge', "
                "'voiceprint_challenge_sample')") == 0);
    const auto carried = client->execSqlSync(
        "SELECT source, model, linked_at FROM voice_profile WHERE user_id = 9");
    REQUIRE(carried.size() == 1);
    CHECK(carried.front()["source"].as<std::string>() == "enrolled");
    CHECK(carried.front()["linked_at"].as<int64_t>() == 1700000000);
  }

  const VoiceprintFeatureService service(testConfig());
  const PassiveEnrollmentService passive(testConfig());
  auto& engine = SpeakerEmbeddingService::instance();

  const auto directory = drogon::sync_wait(service.directory());
  CHECK_FALSE(directory.available);
  CHECK(directory.recognized.empty());

  const bool modelPresent =
      engine.init(ARGUS_TEST_SPEAKER_MODEL) &&
      VoiceprintIndex::instance().init(
          {.dims = engine.dims(), .model = engine.modelId()});
  REQUIRE_MESSAGE(modelPresent, "the speaker model at " ARGUS_TEST_SPEAKER_MODEL " exists but did not load");
  REQUIRE(engine.dims() == 192);

  const PassiveVoiceConfig gates = testConfig().passive;
  const auto embed = [&](const std::vector<int16_t>& samples) {
    return engine.analyze({.voice = pcmOf(samples),
                           .requirement = {.minSpeechSeconds = 1.0F,
                                           .minSnrDb = 0.0F,
                                           .maxClippedRatio = 1.0F},
                           .extractEmbedding = true,
                           .halvesMinSeconds = gates.minHalfSeconds});
  };
  const auto mixedTurn = [] {
    auto samples = turnSamples(
        {.clip = "alpha-2", .offsetSeconds = 0.0F, .seconds = 1.4F, .seed = 7, .repeats = 1});
    const auto tail = turnSamples(
        {.clip = "bravo-2", .offsetSeconds = 0.5F, .seconds = 1.4F, .seed = 8, .repeats = 1});
    samples.insert(samples.end(), tail.begin(), tail.end());
    return samples;
  };

  {
    INFO("the gates sit between what one voice and two voices score");
    std::vector<std::vector<float>> alpha;
    for (const auto& spec : callOf("alpha", 1)) {
      const auto analysis = embed(turnSamples(spec));
      REQUIRE(analysis.status == VoiceAnalysisStatus::Ok);
      if (!analysis.halvesScore) {
        FAIL("expected a value in analysis.halvesScore");
        return;
      }
      MESSAGE(spec.clip << " halves " << *analysis.halvesScore);
      CHECK(*analysis.halvesScore > gates.turnSplitThreshold);
      alpha.push_back(analysis.embedding);
    }
    const auto both = embed(mixedTurn());
    if (!both.halvesScore) {
      FAIL("expected a value in both.halvesScore");
      return;
    }
    MESSAGE("two voices in one turn, halves " << *both.halvesScore);
    CHECK(*both.halvesScore < gates.turnSplitThreshold);
    const std::vector<std::vector<float>> others(alpha.begin() + 1,
                                                 alpha.end());
    CHECK(voice_vector::cosine(alpha[0], voice_vector::centroid(others)) >
          gates.callConsistency);
    const auto stranger = embed(turnSamples(
        {.clip = "charlie-1", .offsetSeconds = 0.0F, .seconds = 3.0F, .seed = 6, .repeats = 2}));
    CHECK(voice_vector::cosine(stranger.embedding,
                               voice_vector::centroid(alpha)) <
          gates.callConsistency);
  }

  const auto oneClipCall = [](const char* clip, uint32_t seed) {
    std::vector<TurnSpec> turns;
    turns.reserve(4);
    for (uint32_t index = 0; index < 4; ++index)
      turns.push_back({.clip = clip,
                       .offsetSeconds = 0.05F * static_cast<float>(index),
                       .seconds = 3.0F,
                       .seed = seed * 31 + index,
                       .repeats = 2});
    return turns;
  };
  const auto profileOf = [](int64_t userId) {
    return count("SELECT COUNT(*) AS total FROM voice_profile WHERE user_id = " +
                 std::to_string(userId) + " AND model <> 'legacy-model'");
  };
  const auto samplesOf = [](int64_t userId) {
    return count("SELECT COUNT(*) AS total FROM voice_sample WHERE user_id = " +
                 std::to_string(userId));
  };
  const auto identifies = [&](const char* clip) {
    const auto found = drogon::sync_wait(service.identify(pcmOf(turnSamples(
        {.clip = clip, .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 99, .repeats = 1}))));
    return found.matched ? found.userId : int64_t{0};
  };

  {
    INFO("Rita is linked after three calls on two days from her own phone");
    const auto first = runCall(passive, {.userId = kRita,
                                         .device = "rita-phone",
                                         .key = "rita-1",
                                         .turns = callOf("alpha", 1),
                                         .at = localTime({.day = 0, .hour = 10})});
    for (const auto& turn : first.turns) {
      CHECK(turn.considered);
      CHECK(turn.quality == VoiceAnalysisStatus::Ok);
      CHECK(turn.verdict == TurnVerdict::Accepted);
    }
    CHECK(first.outcome == PassiveCallOutcome::Pending);
    CHECK(runCall(passive, {.userId = kRita,
                            .device = "rita-phone",
                            .key = "rita-2",
                            .turns = callOf("alpha", 2),
                            .at = localTime({.day = 0, .hour = 14})})
              .outcome == PassiveCallOutcome::Pending);
    CHECK(profileOf(kRita) == 0);
    CHECK(identifies("alpha-4") == 0);

    CHECK(runCall(passive, {.userId = kRita,
                            .device = "rita-phone",
                            .key = "rita-3",
                            .turns = callOf("alpha", 3),
                            .at = localTime({.day = 1, .hour = 10})})
              .outcome == PassiveCallOutcome::Linked);
    CHECK(profileOf(kRita) == 1);
    CHECK(identifies("alpha-4") == kRita);
    CHECK(identifies("bravo-4") == 0);

    const auto listed = drogon::sync_wait(service.directory());
    CHECK(listed.available);
    REQUIRE(listed.recognized.size() == 1);
    CHECK(listed.recognized.front().userId == kRita);
    CHECK(listed.recognized.front().since > localTime({.day = 1, .hour = 10}));

    const auto actions = sink.actions();
    const auto linked = std::ranges::find_if(
        actions, [](const UserActionEvent& event) {
          return event.newData.get("event", "").asString() == "voiceprint_link";
        });
    REQUIRE(linked != actions.end());
    CHECK(linked->recordId == kRita);
    CHECK(linked->action == UserAction::Create);
    CHECK(linked->newData["occasions"].asInt() == 3);
    CHECK(linked->newData["days"].asInt() == 2);
    CHECK_FALSE(linked->newData.isMember("embedding"));
    CHECK(sink.transactional());

    CHECK(count("SELECT COUNT(*) AS total FROM voice_sample "
                "WHERE length(embedding) <> 768") == 0);
    CHECK(count("SELECT COUNT(*) AS total FROM voice_sample "
                "WHERE user_id = 7 AND state = 'adopted'") == 3);
  }

  {
    INFO("another voice in the call stops the call from teaching anything");
    const int64_t before = samplesOf(kRita);
    auto turns = callOf("alpha", 1);
    turns.insert(turns.begin() + 2, oneClipCall("charlie-1", 4).front());
    const auto call = runCall(passive, {.userId = kRita,
                                        .device = "rita-phone",
                                        .key = "rita-tv",
                                        .turns = turns,
                                        .at = localTime({.day = 2, .hour = 9})});
    REQUIRE(call.turns.size() == 5);
    CHECK(call.turns[2].verdict == TurnVerdict::Drift);
    CHECK(call.turns[3].verdict == TurnVerdict::CallTainted);
    CHECK(call.outcome == PassiveCallOutcome::Tainted);
    CHECK(samplesOf(kRita) == before);
  }

  {
    INFO("two voices inside one turn stop the call too");
    const int64_t before = samplesOf(kRita);
    CallInput input{.userId = kRita,
                    .device = "rita-phone",
                    .key = "rita-mixed",
                    .turns = {},
                    .at = localTime({.day = 2, .hour = 15})};
    const auto first = drogon::sync_wait(passive.learnFromTurn(
        {.userId = kRita,
         .deviceHash = input.device,
         .callKey = input.key,
         .sample = pcmOf(turnSamples(callOf("alpha", 2).front())),
         .now = input.at}));
    CHECK(first.verdict == TurnVerdict::Accepted);
    const auto mixed = drogon::sync_wait(
        passive.learnFromTurn({.userId = kRita,
                               .deviceHash = input.device,
                               .callKey = input.key,
                               .sample = pcmOf(mixedTurn()),
                               .now = input.at + 20}));
    CHECK(mixed.verdict == TurnVerdict::MixedTurn);
    CHECK(drogon::sync_wait(passive.closeCall(
              {.callKey = input.key, .now = input.at + 40})) ==
          PassiveCallOutcome::Tainted);
    CHECK(samplesOf(kRita) == before);
  }

  {
    INFO("Rita's linked voice on Gil's account is never learned as Gil's");
    const auto call = runCall(passive, {.userId = kGil,
                                        .device = "gil-phone",
                                        .key = "gil-rita",
                                        .turns = callOf("alpha", 4),
                                        .at = localTime({.day = 3, .hour = 9})});
    REQUIRE_FALSE(call.turns.empty());
    CHECK(call.turns.front().verdict == TurnVerdict::OtherSpeaker);
    const auto& bestOther = call.turns.front().bestOther;
    if (!bestOther) {
      FAIL("the other speaker is missing");
      return;
    }
    CHECK(bestOther->userId == kRita);
    CHECK(call.outcome == PassiveCallOutcome::Tainted);
    CHECK(samplesOf(kGil) == 0);
  }

  {
    INFO("two people taking turns on one account link neither of them");
    for (int day = 4; day < 10; ++day) {
      const bool gil = day % 2 == 0;
      const auto call = runCall(
          passive,
          {.userId = kGil,
           .device = "gil-phone",
           .key = "gil-" + std::to_string(day),
           .turns = gil ? callOf("bravo", static_cast<uint32_t>(day))
                        : oneClipCall("charlie-1", static_cast<uint32_t>(day)),
           .at = localTime({.day = day, .hour = 11})});
      CHECK(call.outcome == PassiveCallOutcome::Pending);
    }
    CHECK(profileOf(kGil) == 0);
    CHECK(samplesOf(kGil) == 6);
  }

  {
    INFO("a tablet two accounts share teaches neither, however consistent");
    CHECK(runCall(passive, {.userId = kGil,
                            .device = "family-tablet",
                            .key = "tablet-gil",
                            .turns = callOf("bravo", 1),
                            .at = localTime({.day = 10, .hour = 18})})
              .outcome == PassiveCallOutcome::Pending);
    for (int day = 11; day < 15; ++day)
      CHECK(runCall(passive,
                    {.userId = kGus,
                     .device = "family-tablet",
                     .key = "tablet-gus-" + std::to_string(day),
                     .turns = callOf("bravo", static_cast<uint32_t>(day)),
                     .at = localTime({.day = day, .hour = 18})})
                .outcome == PassiveCallOutcome::Pending);
    CHECK(profileOf(kGus) == 0);
    CHECK(profileOf(kGil) == 0);
  }

  {
    INFO("Rita's later calls are adopted and refresh her voice in batches");
    std::vector<PassiveCallOutcome> outcomes;
    for (int day = 15; day < 18; ++day)
      outcomes.push_back(
          runCall(passive, {.userId = kRita,
                            .device = "rita-phone",
                            .key = "rita-" + std::to_string(day),
                            .turns = callOf("alpha", static_cast<uint32_t>(day)),
                            .at = localTime({.day = day, .hour = 10})})
              .outcome);
    CHECK(outcomes == std::vector<PassiveCallOutcome>{
                          PassiveCallOutcome::Adopted,
                          PassiveCallOutcome::Adopted,
                          PassiveCallOutcome::Refreshed});
    const auto names = events(sink);
    CHECK(std::ranges::count(names, std::string("voiceprint_refresh")) == 1);
    CHECK(identifies("alpha-4") == kRita);
  }

  {
    INFO("the voice relay reaches all of it over gRPC");
    Fleet fleet;
    REQUIRE(fleet.server);
    const VoiceprintClient client(
        {.target = fleet.target, .fleetSecret = kFleetSecret});
    const VoiceprintClient intruder(
        {.target = fleet.target, .fleetSecret = "wrong"});
    const auto clip = turnSamples(
        {.clip = "alpha-4", .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 3, .repeats = 1});
    const VoiceTurnObservation observation{
        .sample = {.samples = clip, .sampleRate = kRate},
        .userId = kRita,
        .deviceHash = "rita-phone",
        .callKey = "grpc-call",
        .timeoutMs = 5000};
    CHECK_FALSE(intruder.observeTurn(observation).has_value());
    const auto answer = client.observeTurn(observation);
    if (!answer) {
      FAIL("expected a value in answer");
      return;
    }
    CHECK(answer->matched());
    CHECK(answer->user_id() == kRita);
    for (int attempt = 0;
         attempt < 500 && fleet.service.passive().openCalls() == 0; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(fleet.service.passive().openCalls() == 1);
    CHECK(client.closeCall({.callKey = "grpc-call", .timeoutMs = 5000}));
    CHECK(fleet.service.passive().openCalls() == 0);
    CHECK_FALSE(client.closeCall({.callKey = "grpc-call", .timeoutMs = 5000}));
  }

  {
    INFO("only argus-voice may feed or close a call once callers are paired");
    Fleet fleet({{"voice", "voice-identity-credential"},
                 {"guard", "guard-identity-credential"}});
    REQUIRE(fleet.server);
    const VoiceprintClient voice({.target = fleet.target,
                                  .credential = "voice-identity-credential",
                                  .fleetSecret = {}});
    const VoiceprintClient guard({.target = fleet.target,
                                  .credential = "guard-identity-credential",
                                  .fleetSecret = {}});
    const VoiceprintClient legacy(
        {.target = fleet.target, .credential = {}, .fleetSecret = kFleetSecret});
    const auto clip = turnSamples(
        {.clip = "alpha-4", .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 3, .repeats = 1});
    const VoiceTurnObservation observation{
        .sample = {.samples = clip, .sampleRate = kRate},
        .userId = kRita,
        .deviceHash = "rita-phone",
        .callKey = "paired-call",
        .timeoutMs = 5000};
    CHECK_FALSE(guard.observeTurn(observation).has_value());
    CHECK_FALSE(guard.closeCall({.callKey = "paired-call", .timeoutMs = 5000}));
    CHECK_FALSE(legacy.observeTurn(observation).has_value());
    const auto answer = voice.observeTurn(observation);
    if (!answer) {
      FAIL("the paired voice relay answered nothing");
      return;
    }
    CHECK(answer->user_id() == kRita);
    for (int attempt = 0;
         attempt < 500 && fleet.service.passive().openCalls() == 0; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(voice.closeCall({.callKey = "paired-call", .timeoutMs = 5000}));
  }

  {
    INFO("the owner makes Argus forget a voice, and it is gone");
    const auto forgotten =
        drogon::sync_wait(service.forget({.actorId = kOwner, .subjectId = kRita}));
    CHECK(forgotten.hadProfile);
    CHECK(forgotten.samples > 0);
    CHECK(profileOf(kRita) == 0);
    CHECK(samplesOf(kRita) == 0);
    CHECK(identifies("alpha-4") == 0);
    CHECK(drogon::sync_wait(service.directory()).recognized.empty());
    const auto actions = sink.actions();
    const auto forget = std::ranges::find_if(
        actions, [](const UserActionEvent& event) {
          return event.newData.get("event", "").asString() ==
                 "voiceprint_forget";
        });
    REQUIRE(forget != actions.end());
    CHECK(forget->userId == kOwner);
    CHECK(forget->action == UserAction::Delete);

    const auto refusal = [&](int64_t subject) {
      try {
        drogon::sync_wait(
            service.forget({.actorId = kOwner, .subjectId = subject}));
      }
      catch (const ResponseException& error) {
        return error.statusCode();
      }
      return 0;
    };
    CHECK(refusal(kRita) == 404);
    CHECK(refusal(4040) == 404);
    CHECK(samplesOf(kGil) > 0);
  }
}
