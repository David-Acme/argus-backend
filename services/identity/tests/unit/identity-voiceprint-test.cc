#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <app/rpc/identity-voiceprint-rpc-service.hxx>
#include <bit>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <filesystem>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <identity/voiceprint-client.hxx>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <sync/identity-change-sink.hxx>
#include <thread>
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
constexpr int64_t kResident = 7;
constexpr int64_t kGuard = 8;
constexpr int64_t kGuest = 9;

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

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

class ScriptedAuth final : public AuthClient
{
public:
  ScriptedAuth() : AuthClient({.target = "127.0.0.1:9", .fleetSecret = {}}) {}

  [[nodiscard]] std::optional<argus::auth::v1::ValidateTokenResponse>
  validateToken(const ValidateSessionInput& input) const override
  {
    argus::auth::v1::ValidateTokenResponse verdict;
    const auto known = [&](int64_t userId, const char* role) {
      verdict.set_valid(true);
      verdict.mutable_user()->set_user_id(userId);
      verdict.mutable_user()->set_role(role);
    };
    if (input.accessToken == "owner-token")
      known(kOwner, "owner");
    else if (input.accessToken == "resident-token")
      known(kResident, "resident");
    else if (input.accessToken == "guard-token")
      known(kGuard, "guard");
    else
      verdict.set_valid(false);
    return verdict;
  }
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

void put16(std::string& out, uint16_t value)
{
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put32(std::string& out, uint32_t value)
{
  for (unsigned shift = 0; shift < 32U; shift += 8U)
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

EncodedVoice wavOf(const std::vector<int16_t>& samples)
{
  std::string data;
  for (const int16_t sample : samples)
    put16(data, std::bit_cast<uint16_t>(sample));
  std::string wav = "RIFF";
  put32(wav, static_cast<uint32_t>(36 + data.size()));
  wav += "WAVEfmt ";
  put32(wav, 16);
  put16(wav, 1);
  put16(wav, 1);
  put32(wav, 16000);
  put32(wav, 32000);
  put16(wav, 2);
  put16(wav, 16);
  wav += "data";
  put32(wav, static_cast<uint32_t>(data.size()));
  wav += data;
  return {.bytes = std::move(wav),
          .encoding = VoiceEncoding::Wav,
          .sampleRate = 0};
}

std::vector<EncodedVoice> wavs(std::initializer_list<const char*> names)
{
  std::vector<EncodedVoice> voices;
  for (const char* name : names)
    voices.push_back(wavOf(fixture(name)));
  return voices;
}

VoiceprintActor actor(int64_t userId, UserRole role)
{
  return {.userId = userId,
          .role = role,
          .deviceHash = "device-" + std::to_string(userId)};
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

struct Fleet
{
  std::shared_ptr<const AuthClient> auth = std::make_shared<ScriptedAuth>();
  IdentityVoiceprintRpcService service{
      {.fleetSecret = kFleetSecret, .auth = auth, .voiceprint = testConfig()}};
  std::unique_ptr<grpc::Server> server;
  std::string target;

  Fleet()
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

TEST_CASE("a voice is linked once, only under a confirmed enrollment")
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

  const VoiceprintFeatureService service(testConfig());
  auto& engine = SpeakerEmbeddingService::instance();

  {
    INFO("nobody manages another person's voice but the owner");
    const auto forbidden = drogon::sync_wait(
        service.createChallenge({.actor = actor(kGuard, UserRole::Guard),
                                 .subjectId = kResident,
                                 .lang = std::nullopt}));
    CHECK(forbidden.outcome == VoiceprintOutcome::Forbidden);

    const VoiceprintDeleteRequest removal{.actor =
                                              actor(kGuest, UserRole::Guest),
                                          .subjectId = kResident};
    CHECK(drogon::sync_wait(service.remove(removal)).outcome ==
          VoiceprintOutcome::Forbidden);

    const auto missing = drogon::sync_wait(
        service.createChallenge({.actor = actor(kOwner, UserRole::Owner),
                                 .subjectId = 4040,
                                 .lang = std::nullopt}));
    CHECK(missing.outcome == VoiceprintOutcome::UserNotFound);

    const auto inactive = drogon::sync_wait(
        service.createChallenge({.actor = actor(kOwner, UserRole::Owner),
                                 .subjectId = 10,
                                 .lang = std::nullopt}));
    CHECK(inactive.outcome == VoiceprintOutcome::UserNotFound);
  }

  {
    INFO("consent is explicit and versioned");
    for (const auto& [consent, version] :
         {std::pair{false, std::string(kVoiceprintConsentVersion)},
          std::pair{true, std::string("voiceprint-consent-v0")}}) {
      const auto refused = drogon::sync_wait(
          service.enroll({.actor = actor(kResident, UserRole::Resident),
                          .subjectId = kResident,
                          .samples = {},
                          .consent = consent,
                          .consentVersion = version,
                          .challengeId = "whatever",
                          .faceImage = {}}));
      CHECK(refused.outcome == VoiceprintOutcome::ConsentRequired);
    }
  }

  const bool modelPresent =
      engine.init(ARGUS_TEST_SPEAKER_MODEL) &&
      VoiceprintIndex::instance().init(
          {.dims = engine.dims(), .model = engine.modelId()});
  if (!modelPresent) {
    MESSAGE("speaker model not provisioned at " ARGUS_TEST_SPEAKER_MODEL
            "; the model-backed half of the suite is skipped");
    return;
  }
  CHECK(engine.dims() == 192);

  {
    INFO("the same speaker scores far above the threshold, others far below");
    const auto embed = [&](const char* name) {
      const auto analysis = engine.analyze(
          {.voice = wavOf(fixture(name)),
           .requirement = {.minSpeechSeconds = 1.2F, .minSnrDb = 12.0F},
           .extractEmbedding = true});
      REQUIRE(analysis.status == VoiceAnalysisStatus::Ok);
      return analysis.embedding;
    };
    const std::vector<std::vector<float>> alpha{embed("alpha-1"),
                                                embed("alpha-2"),
                                                embed("alpha-3")};
    const auto centroid = voice_vector::centroid(alpha);
    const float same = voice_vector::cosine(centroid, embed("alpha-4"));
    const float other = voice_vector::cosine(centroid, embed("bravo-4"));
    const float stranger = voice_vector::cosine(centroid, embed("charlie-1"));
    MESSAGE("same=" << same << " other=" << other << " stranger=" << stranger);
    CHECK(same > testConfig().verifyThreshold);
    CHECK(other < testConfig().verifyThreshold);
    CHECK(stranger < testConfig().verifyThreshold);
  }

  {
    INFO("enrollment, verification, identification and deletion");
    const auto challenge = drogon::sync_wait(
        service.createChallenge({.actor = actor(kResident, UserRole::Resident),
                                 .subjectId = kResident,
                                 .lang = std::nullopt}));
    REQUIRE(challenge.outcome == VoiceprintOutcome::Ok);
    CHECK(challenge.challenge.lang == VoiceLang::En);
    CHECK(challenge.challenge.phrases.size() == 3);

    const auto enrollAs = [&](const VoiceprintActor& who,
                              std::vector<EncodedVoice> samples,
                              const std::string& challengeId) {
      return drogon::sync_wait(service.enroll(
          {.actor = who,
           .subjectId = kResident,
           .samples = std::move(samples),
           .consent = true,
           .consentVersion = std::string(kVoiceprintConsentVersion),
           .challengeId = challengeId,
           .faceImage = {}}));
    };

    VoiceprintActor otherDevice = actor(kResident, UserRole::Resident);
    otherDevice.deviceHash = "stolen-device";
    CHECK(enrollAs(otherDevice, wavs({"alpha-1", "alpha-2", "alpha-3"}),
                   challenge.challenge.challengeId)
              .outcome == VoiceprintOutcome::ChallengeInvalid);
    CHECK(enrollAs(actor(kResident, UserRole::Resident),
                   wavs({"alpha-1", "alpha-2"}),
                   challenge.challenge.challengeId)
              .outcome == VoiceprintOutcome::SampleCountInvalid);

    const auto mixedUp = enrollAs(actor(kResident, UserRole::Resident),
                                  wavs({"alpha-1", "alpha-2", "bravo-1"}),
                                  challenge.challenge.challengeId);
    CHECK(mixedUp.outcome == VoiceprintOutcome::SamplesInconsistent);
    CHECK(mixedUp.failedSample == 2);

    std::vector<EncodedVoice> silentFirst = wavs({"alpha-2", "alpha-3"});
    silentFirst.insert(silentFirst.begin(),
                       wavOf(std::vector<int16_t>(48000, 0)));
    const auto silent =
        enrollAs(actor(kResident, UserRole::Resident), std::move(silentFirst),
                 challenge.challenge.challengeId);
    CHECK(silent.outcome == VoiceprintOutcome::SampleTooShort);
    CHECK(silent.failedSample == 0);

    const auto enrolled = enrollAs(actor(kResident, UserRole::Resident),
                                   wavs({"alpha-1", "alpha-2", "alpha-3"}),
                                   challenge.challenge.challengeId);
    REQUIRE(enrolled.outcome == VoiceprintOutcome::Ok);
    CHECK(enrolled.status.enrolled);
    CHECK(enrolled.status.sampleCount == 3);
    CHECK(enrolled.status.method == VoiceprintMethod::Self);
    CHECK_FALSE(enrolled.status.stale);

    CHECK(enrollAs(actor(kResident, UserRole::Resident),
                   wavs({"alpha-1", "alpha-2", "alpha-3"}),
                   challenge.challenge.challengeId)
              .outcome == VoiceprintOutcome::AlreadyEnrolled);
    CHECK(drogon::sync_wait(service.createChallenge(
                                {.actor = actor(kResident, UserRole::Resident),
                                 .subjectId = kResident,
                                 .lang = std::nullopt}))
              .outcome == VoiceprintOutcome::AlreadyEnrolled);

    const VoiceprintVerifyRequest genuine{.userId = kResident,
                                          .sample = wavOf(fixture("alpha-4"))};
    const auto verified = drogon::sync_wait(service.verify(genuine));
    CHECK(verified.outcome == VoiceprintOutcome::Ok);
    CHECK(verified.matched);
    const VoiceprintVerifyRequest impostor{.userId = kResident,
                                           .sample = wavOf(fixture("bravo-4"))};
    CHECK_FALSE(drogon::sync_wait(service.verify(impostor)).matched);
    const VoiceprintVerifyRequest unenrolled{.userId = kGuard,
                                             .sample =
                                                 wavOf(fixture("bravo-4"))};
    CHECK(drogon::sync_wait(service.verify(unenrolled)).outcome ==
          VoiceprintOutcome::NotEnrolled);

    const auto known =
        drogon::sync_wait(service.identify(wavOf(fixture("alpha-4"))));
    CHECK(known.matched);
    CHECK(known.userId == kResident);
    CHECK(known.personId == 70);
    CHECK(known.name == "Rita");
    CHECK_FALSE(drogon::sync_wait(service.identify(wavOf(fixture("charlie-1"))))
                    .matched);

    const auto guardChallenge = drogon::sync_wait(
        service.createChallenge({.actor = actor(kGuard, UserRole::Guard),
                                 .subjectId = kGuard,
                                 .lang = VoiceLang::Es}));
    REQUIRE(guardChallenge.outcome == VoiceprintOutcome::Ok);
    const auto taken = drogon::sync_wait(service.enroll(
        {.actor = actor(kGuard, UserRole::Guard),
         .subjectId = kGuard,
         .samples = wavs({"alpha-2", "alpha-3", "alpha-4"}),
         .consent = true,
         .consentVersion = std::string(kVoiceprintConsentVersion),
         .challengeId = guardChallenge.challenge.challengeId,
         .faceImage = {}}));
    CHECK(taken.outcome == VoiceprintOutcome::VoiceTaken);

    const auto assisted = drogon::sync_wait(
        service.createChallenge({.actor = actor(kOwner, UserRole::Owner),
                                 .subjectId = kGuest,
                                 .lang = std::nullopt}));
    REQUIRE(assisted.outcome == VoiceprintOutcome::Ok);
    const auto faceless = drogon::sync_wait(service.enroll(
        {.actor = actor(kOwner, UserRole::Owner),
         .subjectId = kGuest,
         .samples = wavs({"bravo-1", "bravo-2", "bravo-3"}),
         .consent = true,
         .consentVersion = std::string(kVoiceprintConsentVersion),
         .challengeId = assisted.challenge.challengeId,
         .faceImage = "not a face"}));
    CHECK(faceless.outcome == VoiceprintOutcome::FaceNotVerified);

    const VoiceprintDeleteRequest byOwner{.actor =
                                              actor(kOwner, UserRole::Owner),
                                          .subjectId = kResident};
    const auto removed = drogon::sync_wait(service.remove(byOwner));
    CHECK(removed.outcome == VoiceprintOutcome::Ok);
    CHECK(removed.deleted);
    CHECK(drogon::sync_wait(service.remove(byOwner)).outcome ==
          VoiceprintOutcome::NotEnrolled);
    CHECK_FALSE(
        drogon::sync_wait(service.identify(wavOf(fixture("alpha-4")))).matched);

    const auto audit = sink.actions();
    REQUIRE(audit.size() == 2);
    CHECK(audit[0].action == UserAction::Create);
    CHECK(audit[0].userId == kResident);
    CHECK(audit[0].recordId == kResident);
    CHECK(audit[0].newData["event"].asString() == "voiceprint_enroll");
    CHECK(audit[1].action == UserAction::Delete);
    CHECK(audit[1].userId == kOwner);
    CHECK(audit[1].newData["byOwner"].asBool());
    for (const auto& event : audit) {
      CHECK_FALSE(event.newData.isMember("embedding"));
      CHECK(event.newData.toStyledString().size() < 512);
    }
    CHECK(sink.transactional());
  }

  {
    INFO("the app's flow: one phrase at a time, then a confirmation");
    const VoiceprintActor resident = actor(kResident, UserRole::Resident);
    const VoiceprintChallengeRequest challengeRequest{.actor = resident,
                                                      .subjectId = kResident,
                                                      .lang = VoiceLang::Es};
    const auto challenge =
        drogon::sync_wait(service.createChallenge(challengeRequest));
    REQUIRE(challenge.outcome == VoiceprintOutcome::Ok);
    const std::string& challengeId = challenge.challenge.challengeId;

    const auto stage = [&](int position, const char* name) {
      return drogon::sync_wait(
          service.stageSample({.actor = resident,
                               .subjectId = kResident,
                               .challengeId = challengeId,
                               .position = position,
                               .sample = wavOf(fixture(name))}));
    };
    const auto finalize = [&]() {
      const VoiceprintFinalizeRequest request{.actor = resident,
                                              .subjectId = kResident,
                                              .consent = true,
                                              .consentVersion = std::string(
                                                  kVoiceprintConsentVersion),
                                              .challengeId = challengeId,
                                              .faceImage = {}};
      return drogon::sync_wait(service.finalize(request));
    };

    const auto first = stage(0, "alpha-1");
    CHECK(first.outcome == VoiceprintOutcome::Ok);
    CHECK(first.collected == 1);
    CHECK(first.required == 3);
    CHECK(first.speechSeconds > 1.2F);

    const auto silent = drogon::sync_wait(
        service.stageSample({.actor = resident,
                             .subjectId = kResident,
                             .challengeId = challengeId,
                             .position = 1,
                             .sample = wavOf(std::vector<int16_t>(48000, 0))}));
    CHECK(silent.outcome == VoiceprintOutcome::SampleTooShort);
    CHECK(silent.collected == 1);

    CHECK(stage(3, "alpha-2").outcome == VoiceprintOutcome::SampleCountInvalid);
    VoiceprintActor elsewhere = resident;
    elsewhere.deviceHash = "another-device";
    CHECK(drogon::sync_wait(
              service.stageSample({.actor = elsewhere,
                                   .subjectId = kResident,
                                   .challengeId = challengeId,
                                   .position = 1,
                                   .sample = wavOf(fixture("alpha-2"))}))
              .outcome == VoiceprintOutcome::ChallengeInvalid);

    CHECK(stage(1, "alpha-2").collected == 2);
    CHECK(finalize().outcome == VoiceprintOutcome::SampleCountInvalid);

    CHECK(stage(2, "bravo-2").collected == 3);
    const auto mixed = finalize();
    CHECK(mixed.outcome == VoiceprintOutcome::SamplesInconsistent);
    CHECK(mixed.failedSample == 2);

    CHECK(stage(2, "alpha-3").collected == 3);
    const auto linked = finalize();
    REQUIRE(linked.outcome == VoiceprintOutcome::Ok);
    CHECK(linked.status.sampleCount == 3);
    CHECK(finalize().outcome == VoiceprintOutcome::AlreadyEnrolled);

    const auto leftovers = DbService::identityClient()->execSqlSync(
        "SELECT COUNT(*) AS total FROM voiceprint_challenge_sample");
    CHECK(leftovers.front()["total"].as<int>() == 0);

    const VoiceprintDeleteRequest bySelf{.actor = resident,
                                         .subjectId = kResident};
    CHECK(drogon::sync_wait(service.remove(bySelf)).deleted);
  }

  {
    INFO("the SDK reaches the same surface over gRPC, gated by the session");
    Fleet fleet;
    REQUIRE(fleet.server);
    const VoiceprintClient client(
        {.target = fleet.target, .fleetSecret = kFleetSecret});
    const VoiceprintSession resident{.accessToken = "resident-token",
                                     .deviceHash = "phone-7"};

    const auto status = client.status(kResident);
    if (!status) {
      FAIL("status answered nothing");
      return;
    }
    CHECK(status->available());
    CHECK_FALSE(status->status().enrolled());

    CHECK_FALSE(client
                    .createChallenge({.userId = kResident,
                                      .lang = "",
                                      .session = {.accessToken = "forged",
                                                  .deviceHash = ""}})
                    .has_value());

    const auto challenge = client.createChallenge(
        {.userId = kResident, .lang = "es", .session = resident});
    if (!challenge) {
      FAIL("challenge answered nothing");
      return;
    }
    REQUIRE(challenge->outcome() == argus::identity::v1::VOICEPRINT_OK);
    CHECK(challenge->lang() == "es");

    const auto alpha1 = fixture("alpha-1");
    const auto alpha2 = fixture("alpha-2");
    const auto alpha3 = fixture("alpha-3");
    const auto enrolled =
        client.enroll({.userId = kResident,
                       .samples = {{.samples = alpha1, .sampleRate = 16000},
                                   {.samples = alpha2, .sampleRate = 16000},
                                   {.samples = alpha3, .sampleRate = 16000}},
                       .consent = true,
                       .consentVersion = std::string(kVoiceprintConsentVersion),
                       .challengeId = challenge->challenge_id(),
                       .faceImage = "",
                       .session = resident});
    if (!enrolled) {
      FAIL("enrolled answered nothing");
      return;
    }
    REQUIRE(enrolled->outcome() == argus::identity::v1::VOICEPRINT_OK);
    CHECK(enrolled->status().method() == "self");

    const auto alpha4 = fixture("alpha-4");
    const auto identified = client.identifyWithin(
        {.sample = {.samples = alpha4, .sampleRate = 16000},
         .timeoutMs = 3000});
    if (!identified) {
      FAIL("identified answered nothing");
      return;
    }
    CHECK(identified->matched());
    CHECK(identified->user_id() == kResident);
    CHECK(identified->role() == "resident");

    const auto bravo4 = fixture("bravo-4");
    const auto verdict =
        client.verify({.userId = kResident,
                       .sample = {.samples = bravo4, .sampleRate = 16000}});
    if (!verdict) {
      FAIL("verdict answered nothing");
      return;
    }
    CHECK_FALSE(verdict->matched());

    const auto byGuard = client.remove(
        {.userId = kResident,
         .session = {.accessToken = "guard-token", .deviceHash = ""}});
    if (!byGuard) {
      FAIL("byGuard answered nothing");
      return;
    }
    CHECK(byGuard->outcome() == argus::identity::v1::VOICEPRINT_FORBIDDEN);

    const auto bySelf =
        client.remove({.userId = kResident, .session = resident});
    if (!bySelf) {
      FAIL("bySelf answered nothing");
      return;
    }
    CHECK(bySelf->deleted());

    const VoiceprintClient stranger(
        {.target = fleet.target, .fleetSecret = ""});
    CHECK_FALSE(stranger.status(kResident).has_value());
  }
}
