#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-voiceprint-rpc-service.hxx>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
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
#include <random>
#include <span>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <test-support/app-runner.hxx>
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

const std::string& dbPath()
{
  static const std::string path =
      (std::filesystem::temp_directory_path() / "identity-voiceprint-verdict-test.db").string();
  return path;
}
constexpr const char* kFleetSecret = "voiceprint-verdict-secret";
constexpr int64_t kAda = 1;
constexpr int64_t kRita = 7;
constexpr int64_t kGil = 8;
constexpr int kRate = 16000;

using test_support::AppRunner;

IdentityVoiceprintConfig testConfig()
{
  IdentityVoiceprintConfig config;
  config.modelPath = ARGUS_TEST_SPEAKER_MODEL;
  return config;
}

IdentityVoiceprintConfig verdictOnlyConfig()
{
  IdentityVoiceprintConfig config = testConfig();
  config.passive.enabled = false;
  return config;
}

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

void boot()
{
  static const auto runner = [] {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((dbPath() + suffix).c_str());
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                         .filename = dbPath(),
                                                         .name = "default",
                                                         .timeout = -1});
    VecDb::instance().setDbFile(dbPath());
    return std::make_unique<AppRunner>();
  }();
  REQUIRE(runner != nullptr);
  REQUIRE(waitForBoot());
  static const bool schema = [] {
    DbService::installExtensions();
    return DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA);
  }();
  REQUIRE(schema);
  static const bool seeded = [] {
    auto client = DbService::identityClient();
    client->execSqlSync(
        "INSERT INTO user (id, name, last_name, role, lang, is_active) VALUES "
        "(1, 'Ada', 'Owner', 'owner', 'es', 1), "
        "(7, 'Rita', 'Resident', 'resident', 'es', 1), "
        "(8, 'Gil', 'Guard', 'guard', 'es', 1)");
    client->execSqlSync(
        "INSERT INTO user_privacy (user_id, notice_version, presence, "
        "face_cameras, voice_learning, camera_audio) "
        "SELECT id, 1, 1, 1, 1, 1 FROM user");
    return true;
  }();
  REQUIRE(seeded);
}

struct EngineTeardown
{
  EngineTeardown() = default;
  ~EngineTeardown() { SpeakerEmbeddingService::instance().shutdown(); }

  EngineTeardown(const EngineTeardown&) = delete;
  EngineTeardown& operator=(const EngineTeardown&) = delete;
};

bool loaded()
{
  static const bool ready = [] {
    auto& engine = SpeakerEmbeddingService::instance();
    return engine.init(ARGUS_TEST_SPEAKER_MODEL) &&
           VoiceprintIndex::instance().init(
               {.dims = engine.dims(), .model = engine.modelId()});
  }();
  static const EngineTeardown teardown;
  return ready;
}

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
  float seconds{3.0F};
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

VoiceAnalysisInput analysisInput(const std::vector<int16_t>& samples)
{
  const auto config = testConfig();
  return {.voice = pcmOf(samples),
          .requirement = {.minSpeechSeconds = config.minVerifySpeechSeconds,
                          .minSnrDb = config.minSnrDb,
                          .maxClippedRatio = speech_quality::kMaxClippedRatio},
          .extractEmbedding = true,
          .halvesMinSeconds = std::nullopt};
}

std::vector<float> embeddingOf(const std::vector<int16_t>& samples)
{
  const auto analysis =
      SpeakerEmbeddingService::instance().analyze(analysisInput(samples));
  REQUIRE(analysis.status == VoiceAnalysisStatus::Ok);
  return analysis.embedding;
}

void reindex()
{
  auto& engine = SpeakerEmbeddingService::instance();
  REQUIRE(VoiceprintIndex::instance().init(
      {.dims = engine.dims(), .model = engine.modelId()}));
}

void putRawProfile(int64_t userId, std::vector<char> blob)
{
  const auto now = static_cast<int64_t>(std::time(nullptr));
  static_cast<void>(drogon::sync_wait(VoiceProfileRepository().upsert(
      {.userId = userId,
       .model = SpeakerEmbeddingService::instance().modelId(),
       .embedding = std::move(blob),
       .sampleCount = 3,
       .speechSeconds = 9.0,
       .source = VoiceProfileSource::Enrolled,
       .linkedAt = now,
       .refreshedAt = now})));
}

void putProfile(int64_t userId, std::span<const float> embedding)
{
  putRawProfile(userId, voice_vector::toBlob(embedding));
}

struct FixtureProfiles
{
  std::vector<float> alpha;
  std::vector<float> bravo;
};

const FixtureProfiles& fixtureProfiles()
{
  static const FixtureProfiles profiles = [] {
    const auto embed = [](const char* name, uint32_t seed) {
      return embeddingOf(turnSamples({.clip = name,
                                      .offsetSeconds = 0.0F,
                                      .seconds = 3.0F,
                                      .seed = seed,
                                      .repeats = 1}));
    };
    const std::vector<std::vector<float>> alpha{
        embed("alpha-1", 11), embed("alpha-2", 12), embed("alpha-3", 13)};
    const std::vector<std::vector<float>> bravo{
        embed("bravo-1", 21), embed("bravo-2", 22), embed("bravo-3", 23)};
    return FixtureProfiles{.alpha = voice_vector::centroid(alpha),
                           .bravo = voice_vector::centroid(bravo)};
  }();
  return profiles;
}

void freshWorld()
{
  REQUIRE(loaded());
  auto client = DbService::identityClient();
  client->execSqlSync("UPDATE user SET is_active = 1");
  client->execSqlSync("UPDATE user_privacy SET voice_learning = 1");
  client->execSqlSync(
      "UPDATE household_privacy SET voice_learning = 1 WHERE id = 1");
  const auto& profiles = fixtureProfiles();
  const auto dims = static_cast<size_t>(SpeakerEmbeddingService::instance().dims());
  REQUIRE(profiles.alpha.size() == dims);
  REQUIRE(profiles.bravo.size() == dims);
  putProfile(kRita, profiles.alpha);
  putProfile(kGil, profiles.bravo);
  reindex();
}

std::vector<float> profileAt(std::span<const float> probe, float target)
{
  const auto unit = voice_vector::normalized(probe);
  size_t weakest = 0;
  for (size_t index = 1; index < unit.size(); ++index)
    if (std::fabs(unit[index]) < std::fabs(unit[weakest]))
      weakest = index;
  std::vector<float> axis(unit.size(), 0.0F);
  axis[weakest] = 1.0F;
  float projection = 0.0F;
  for (size_t index = 0; index < unit.size(); ++index)
    projection += axis[index] * unit[index];
  for (size_t index = 0; index < unit.size(); ++index)
    axis[index] -= projection * unit[index];
  const auto normal = voice_vector::normalized(axis);
  std::vector<float> profile(unit.size(), 0.0F);
  const float angle = std::acos(target);
  for (size_t index = 0; index < unit.size(); ++index)
    profile[index] =
        std::cos(angle) * unit[index] + std::sin(angle) * normal[index];
  return voice_vector::normalized(profile);
}

void shapeHolder(float score, std::span<const float> probe)
{
  putProfile(kRita, profileAt(probe, score));
  reindex();
}

struct PosedProfiles
{
  std::span<const float> probe;
  float other{0.0F};
  float holder{0.0F};
};

void poseProfiles(const PosedProfiles& posed)
{
  putProfile(kGil, profileAt(posed.probe, posed.other));
  putProfile(kRita, profileAt(posed.probe, posed.holder));
  reindex();
}

void setConsent(int64_t userId, bool effective)
{
  DbService::identityClient()->execSqlSync(
      "UPDATE user_privacy SET voice_learning = ? WHERE user_id = ?",
      effective ? 1 : 0, userId);
}

void setHousehold(bool effective)
{
  DbService::identityClient()->execSqlSync(
      "UPDATE household_privacy SET voice_learning = ? WHERE id = 1",
      effective ? 1 : 0);
}

VoiceprintFeatureService& service()
{
  static VoiceprintFeatureService instance(testConfig());
  return instance;
}

VoiceprintIdentifyResult identifyAs(int64_t holderId,
                                    const std::vector<int16_t>& samples)
{
  return drogon::sync_wait(
      service().identify({.sample = pcmOf(samples), .holderId = holderId}));
}

std::vector<int16_t> ritaSpeaking()
{
  return turnSamples(
      {.clip = "alpha-4", .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 5});
}

std::vector<int16_t> gilSpeaking()
{
  return turnSamples(
      {.clip = "bravo-4", .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 6});
}

std::vector<int16_t> strangerSpeaking()
{
  return turnSamples(
      {.clip = "charlie-1", .offsetSeconds = 0.0F, .seconds = 3.0F, .seed = 7});
}

struct Fleet
{
  IdentityVoiceprintRpcService service;
  std::unique_ptr<grpc::Server> server;
  std::string target;

  Fleet()
      : service({.gate = std::make_shared<const argus::client::FleetCallerGate>(
                     argus::client::FleetGateConfig{
                         .expectedCallers = identity_callers::expected(),
                         .callerPairs = {},
                         .legacySecret = kFleetSecret,
                         .onFirstLegacy = {}}),
                 .voiceprint = verdictOnlyConfig()})
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

TEST_CASE("the holder's own voice is HOLDER, and the holder score says so" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  const auto found = identifyAs(kRita, ritaSpeaking());
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score " << *found.holderScore);
  CHECK(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Holder);
  CHECK(*found.holderScore >= testConfig().identifyThreshold);
  REQUIRE(found.holderProfile.has_value());
  CHECK(*found.holderProfile);
  CHECK(found.matched);
  CHECK(found.userId == kRita);
}

TEST_CASE("another enrolled voice is OTHER_KNOWN and names the person who "
          "spoke" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  const auto found = identifyAs(kRita, gilSpeaking());
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score of another voice " << *found.holderScore);
  CHECK(found.matched);
  CHECK(found.userId == kGil);
  CHECK(found.name == "Gil");
  CHECK(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::OtherKnown);
}

TEST_CASE("a voice nobody enrolled is UNFAMILIAR, below the ceiling" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  const auto found = identifyAs(kRita, strangerSpeaking());
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score of a stranger " << *found.holderScore);
  REQUIRE(found.holderProfile.has_value());
  CHECK(*found.holderProfile);
  CHECK(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unfamiliar);
  CHECK(*found.holderScore < testConfig().unfamiliarCeiling);
}

TEST_CASE("a holder who never enrolled is UNKNOWN, never UNFAMILIAR" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  const auto found = identifyAs(kAda, strangerSpeaking());
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderProfile.has_value());
  CHECK_FALSE(*found.holderProfile);
  CHECK_FALSE(found.holderScore.has_value());
  CHECK(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("a score in the grey band is UNKNOWN, never a guess" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  const auto probe = strangerSpeaking();
  shapeHolder(0.47F, embeddingOf(probe));

  const auto found = identifyAs(kRita, probe);
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderScore.has_value());
  MESSAGE("grey-band holder score " << *found.holderScore);
  CHECK(*found.holderScore >= testConfig().unfamiliarCeiling);
  CHECK(*found.holderScore < testConfig().identifyThreshold);
  CHECK(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("another enrolled voice outranks a holder score over the threshold" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  const auto probe = gilSpeaking();
  const auto embedding = embeddingOf(probe);
  poseProfiles({.probe = embedding, .other = 0.80F, .holder = 0.60F});

  const auto found = identifyAs(kRita, probe);
  REQUIRE(found.matched);
  CHECK(found.userId == kGil);
  CHECK(found.name == "Gil");
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score behind a confident other voice " << *found.holderScore);
  CHECK(*found.holderScore >= testConfig().identifyThreshold);
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::OtherKnown);
}

TEST_CASE("a 1:N pair inside the margin leaves the holder's own score to "
          "decide" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  const auto probe = gilSpeaking();
  const auto embedding = embeddingOf(probe);
  poseProfiles({.probe = embedding, .other = 0.70F, .holder = 0.68F});

  const auto found = identifyAs(kRita, probe);
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score inside the runner-up margin " << *found.holderScore);
  CHECK(*found.holderScore >= testConfig().identifyThreshold);
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Holder);
}

TEST_CASE("a confident best the gates suppressed is UNKNOWN, never HOLDER, "
          "and names nobody" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  const auto probe = gilSpeaking();
  const auto embedding = embeddingOf(probe);
  poseProfiles({.probe = embedding, .other = 0.80F, .holder = 0.60F});
  DbService::identityClient()
      ->execSqlSync("UPDATE user SET is_active = 0 WHERE id = ?", kGil);

  const auto found = identifyAs(kRita, probe);
  CHECK_FALSE(found.matched);
  CHECK(found.userId == 0);
  CHECK(found.name.empty());
  REQUIRE(found.holderScore.has_value());
  MESSAGE("holder score behind a suppressed confident best "
          << *found.holderScore);
  CHECK(*found.holderScore >= testConfig().identifyThreshold);
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("a holder profile written for another model is UNKNOWN" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  DbService::identityClient()
      ->execSqlSync("UPDATE voice_profile SET model = ? WHERE user_id = ?",
                    std::string("stale-model"), kRita);

  const auto found = identifyAs(kRita, strangerSpeaking());
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderProfile.has_value());
  CHECK_FALSE(*found.holderProfile);
  CHECK_FALSE(found.holderScore.has_value());
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("a truncated holder profile is UNKNOWN, never UNFAMILIAR" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  putRawProfile(kRita, std::vector<char>(sizeof(float), 0));

  const auto found = identifyAs(kRita, strangerSpeaking());
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderProfile.has_value());
  CHECK(*found.holderProfile);
  CHECK_FALSE(found.holderScore.has_value());
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("a holder profile with a zero norm is UNKNOWN, never UNFAMILIAR" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  const std::vector<float>
      zeros(static_cast<size_t>(SpeakerEmbeddingService::instance().dims()),
            0.0F);
  putRawProfile(kRita, voice_vector::toBlob(zeros));

  const auto found = identifyAs(kRita, strangerSpeaking());
  CHECK_FALSE(found.matched);
  REQUIRE(found.holderProfile.has_value());
  CHECK(*found.holderProfile);
  CHECK_FALSE(found.holderScore.has_value());
  REQUIRE(found.verdict.has_value());
  CHECK(*found.verdict == VoiceprintVerdict::Unknown);
}

TEST_CASE("Identify omits the holder fields and ObserveTurn fills them" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  Fleet fleet;
  REQUIRE(fleet.server);
  const VoiceprintClient client(
      {.target = fleet.target, .fleetSecret = kFleetSecret});
  const auto clip = turnSamples(
      {.clip = "alpha-4", .offsetSeconds = 0.0F, .seconds = 2.9F, .seed = 9});
  const VoiceClipView view{.samples = clip, .sampleRate = kRate};

  const auto spoken = client.identify(view);
  REQUIRE(spoken.has_value());
  CHECK(spoken->matched());
  CHECK(spoken->user_id() == kRita);
  CHECK_FALSE(spoken->has_holder_score());
  CHECK_FALSE(spoken->has_holder_profile());
  CHECK_FALSE(spoken->has_verdict());

  const auto turn = client.observeTurn({.sample = view,
                                        .userId = kRita,
                                        .deviceHash = "rita-phone",
                                        .callKey = "verdict-call",
                                        .timeoutMs = 5000});
  REQUIRE(turn.has_value());
  CHECK(turn->matched());
  CHECK(turn->user_id() == kRita);
  REQUIRE(turn->has_holder_score());
  MESSAGE("holder score on the wire " << turn->holder_score());
  CHECK(turn->holder_score() >= testConfig().identifyThreshold);
  REQUIRE(turn->has_holder_profile());
  CHECK(turn->holder_profile());
  REQUIRE(turn->has_verdict());
  CHECK(turn->verdict() == argus::identity::v1::VOICEPRINT_VERDICT_HOLDER);
  CHECK(fleet.service.passive().openCalls() == 0);
}

TEST_CASE("the wire carries an explicit UNKNOWN, apart from an absent "
          "verdict" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  Fleet fleet;
  REQUIRE(fleet.server);
  const VoiceprintClient client(
      {.target = fleet.target, .fleetSecret = kFleetSecret});
  const auto clip = strangerSpeaking();
  const VoiceClipView view{.samples = clip, .sampleRate = kRate};

  CHECK(static_cast<int>(argus::identity::v1::VOICEPRINT_VERDICT_UNSPECIFIED) ==
        0);
  CHECK(static_cast<int>(argus::identity::v1::VOICEPRINT_VERDICT_UNKNOWN) == 4);

  const auto turn = client.observeTurn({.sample = view,
                                        .userId = kAda,
                                        .deviceHash = "ada-phone",
                                        .callKey = "unknown-call",
                                        .timeoutMs = 5000});
  REQUIRE(turn.has_value());
  REQUIRE(turn->has_verdict());
  CHECK(turn->verdict() == argus::identity::v1::VOICEPRINT_VERDICT_UNKNOWN);
  REQUIRE(turn->has_holder_profile());
  CHECK_FALSE(turn->holder_profile());
  CHECK_FALSE(turn->has_holder_score());

  const auto spoken = client.identify(view);
  REQUIRE(spoken.has_value());
  CHECK_FALSE(spoken->has_verdict());
  CHECK(fleet.service.passive().openCalls() == 0);
}

TEST_CASE("a holder who has not consented answers without a verdict" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  setConsent(kRita, false);

  const auto found = identifyAs(kRita, ritaSpeaking());
  CHECK_FALSE(found.holderScore.has_value());
  CHECK_FALSE(found.holderProfile.has_value());
  CHECK_FALSE(found.verdict.has_value());
  CHECK_FALSE(found.matched);
  CHECK(found.outcome == VoiceprintOutcome::Ok);
  CHECK(found.threshold == testConfig().identifyThreshold);
}

TEST_CASE("the household switch off answers without a verdict" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();
  setHousehold(false);

  const auto found = identifyAs(kRita, ritaSpeaking());
  CHECK_FALSE(found.holderScore.has_value());
  CHECK_FALSE(found.holderProfile.has_value());
  CHECK_FALSE(found.verdict.has_value());
  CHECK_FALSE(found.matched);
  CHECK(found.outcome == VoiceprintOutcome::Ok);
  CHECK(found.threshold == testConfig().identifyThreshold);
}

TEST_CASE("an unusable clip answers UNKNOWN with the rest of the response "
          "intact" *
          doctest::skip(!std::filesystem::exists(ARGUS_TEST_SPEAKER_MODEL)))
{
  boot();
  freshWorld();

  const auto found = identifyAs(
      kRita, turnSamples({.clip = "alpha-4", .offsetSeconds = 0.0F,
                          .seconds = 0.3F, .seed = 8}));
  CHECK(found.outcome == VoiceprintOutcome::SampleTooShort);
  CHECK_FALSE(found.verdict.has_value());
  CHECK_FALSE(found.holderScore.has_value());
  CHECK_FALSE(found.holderProfile.has_value());
  CHECK(found.threshold == testConfig().identifyThreshold);
}
