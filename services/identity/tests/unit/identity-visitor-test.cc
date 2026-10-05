#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/retention/services/candidate-retention-service.hxx>
#include <feature/visitor/services/visitor-feature-service.hxx>
#include <feature/visitor/services/visitor-recognition-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/vocabulary/face-model.hxx>

#include <chrono>
#include <cstdio>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <thread>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif
#ifndef ARGUS_TEST_FACE_MODELS
#error "ARGUS_TEST_FACE_MODELS must point at models/face"
#endif
#ifndef ARGUS_TEST_FACE_FIXTURES
#error "ARGUS_TEST_FACE_FIXTURES must point at tests/fixtures/face"
#endif

namespace
{
constexpr const char* kDb = "identity-visitor-test.db";
constexpr int64_t kCamera = 6;

std::string fixture(const std::string& name)
{
  std::ifstream in(std::string(ARGUS_TEST_FACE_FIXTURES) + "/" + name,
                   std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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

int64_t count(const std::string& sql)
{
  return DbService::client()->execSqlSync(sql).front()["n"].as<int64_t>();
}

SightingResult observe(VisitorRecognitionService& service, const std::string& name,
                       int64_t at)
{
  return drogon::sync_wait(service.observe(
      {.image = fixture(name), .cameraId = kCamera, .observedAt = at}));
}

void enrollHousehold(const std::string& name)
{
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(1, 'Jess', '', 'owner')");
  client->execSqlSync("INSERT INTO person (id, user_id, name, status) VALUES "
                      "(1, 1, 'Jess', 'known')");
  const auto analysis = FaceService::instance().analyzeImage(
      {.imageBytes = fixture(name), .encodeFace = false});
  if (!analysis) {
    FAIL("the household fixture has no face");
    return;
  }
  const auto& embedding = analysis->embedding;
  const std::vector<char> blob(
      reinterpret_cast<const char*>(embedding.data()),
      reinterpret_cast<const char*>(embedding.data()) +
          embedding.size() * sizeof(float));
  const auto row = client->execSqlSync(
      "INSERT INTO face_embedding (person_id, embedding, model) VALUES (1, ?, ?)",
      blob, std::string(kFaceModelId));
  REQUIRE(FaceService::instance().faceDb().insert(
      {.embedding = embedding.data(),
       .personId = 1,
       .faceEmbeddingId = static_cast<int64_t>(row.insertId())}));
}
}

TEST_CASE("recurring unknown faces become numbered visitors, the household never "
          "does, and the Owner can name, merge, split and delete them")
{
  if (!std::filesystem::exists(std::string(ARGUS_TEST_FACE_MODELS) +
                               "/recognizer.bin")) {
    MESSAGE("face models not provisioned; skipped");
    return;
  }
  for (const char* suffix : {"", "-wal", "-shm"})
    std::remove((std::string(kDb) + suffix).c_str());
  VecDb::instance().setDbFile(kDb);
  FaceService::instance().init(ARGUS_TEST_FACE_MODELS);
  REQUIRE(FaceService::instance().isLoaded());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot());
  DbService::installExtensions();
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  enrollHousehold("meir-a.jpg");

  VisitorRecognitionService recognition;
  const int64_t t0 = 1'790'000'000;

  const auto off = observe(recognition, "barratt-a.jpg", t0);
  CHECK(off.faceFound);
  CHECK(off.decision.outcome == SightingOutcome::Disabled);
  CHECK(count("SELECT COUNT(*) AS n FROM person") == 1);

  DbService::client()->execSqlSync(
      "UPDATE household_privacy SET visitor_recognition = 1 WHERE id = 1");

  const auto first = observe(recognition, "barratt-a.jpg", t0);
  CHECK(first.decision.outcome == SightingOutcome::NewVisitor);
  CHECK(first.created);
  CHECK(first.newVisit);
  REQUIRE(first.visitorNumber.has_value());
  const int64_t barratt = first.decision.personId;

  const auto again = observe(recognition, "barratt-b.jpg", t0 + 120);
  CHECK(again.decision.outcome == SightingOutcome::Visitor);
  CHECK(again.decision.personId == barratt);
  CHECK_FALSE(again.newVisit);
  CHECK(again.visits == 1);

  const auto nextDay = observe(recognition, "barratt-b.jpg", t0 + 86400);
  CHECK(nextDay.decision.personId == barratt);
  CHECK(nextDay.newVisit);
  CHECK(nextDay.visits == 2);

  const auto owner = observe(recognition, "meir-b.jpg", t0 + 200);
  CHECK(owner.decision.outcome == SightingOutcome::Household);
  CHECK(owner.decision.personId == 1);
  CHECK(count("SELECT COUNT(*) AS n FROM face_embedding WHERE person_id = 1") == 1);

  const auto menon = observe(recognition, "menon.jpg", t0 + 300);
  CHECK(menon.decision.outcome == SightingOutcome::NewVisitor);
  CHECK(menon.decision.personId != barratt);
  const auto hathaway = observe(recognition, "hathaway.jpg", t0 + 400);
  CHECK(hathaway.decision.outcome == SightingOutcome::NewVisitor);

  CHECK(count("SELECT COUNT(*) AS n FROM person WHERE user_id IS NULL") == 3);
  CHECK(count("SELECT COUNT(*) AS n FROM person_visit") == 4);

  const VisitorFeatureService gallery;
  const VisitorRequester ownerActor{.userId = 1, .role = UserRole::Owner};
  const VisitorRequester guardActor{.userId = 2, .role = UserRole::Guard};

  const auto listed = drogon::sync_wait(
      gallery.list({.requester = ownerActor, .namedOnly = false}));
  CHECK(listed.recognitionEnabled);
  CHECK(listed.visitors.size() == 3);
  CHECK(drogon::sync_wait(gallery.list({.requester = guardActor, .namedOnly = false}))
            .visitors.empty());

  const auto named = drogon::sync_wait(gallery.update(
      {.requester = ownerActor,
       .personId = barratt,
       .body = {.name = "Mike",
                .categoryValue = "neighbor",
                .note = std::nullopt,
                .category = PersonCategory::Neighbor}}));
  CHECK(named.visitor.name == "Mike");
  CHECK(named.visitor.category == PersonCategory::Neighbor);
  CHECK(named.visitor.visitCount == 2);
  CHECK(named.visits.size() == 2);
  CHECK(drogon::sync_wait(gallery.list({.requester = guardActor, .namedOnly = false}))
            .visitors.size() == 1);

  const auto trusted = observe(recognition, "barratt-a.jpg", t0 + 90000);
  CHECK(trusted.decision.outcome == SightingOutcome::Visitor);
  CHECK(trusted.named);
  CHECK(trusted.known);
  CHECK(trusted.category == PersonCategory::Neighbor);

  const auto merged = drogon::sync_wait(gallery.merge(
      {.requester = ownerActor,
       .personId = barratt,
       .body = {.sourceIds = {menon.decision.personId}}}));
  CHECK(merged.visitor.visitCount == 4);
  CHECK(count("SELECT COUNT(*) AS n FROM person WHERE user_id IS NULL") == 2);
  CHECK_THROWS(drogon::sync_wait(gallery.merge(
      {.requester = ownerActor, .personId = barratt, .body = {.sourceIds = {1}}})));

  const auto menonRows = DbService::client()->execSqlSync(
      "SELECT f.id FROM face_embedding f JOIN person_visit v ON v.person_id = "
      "f.person_id WHERE f.person_id = ? ORDER BY f.id DESC LIMIT 1",
      barratt);
  REQUIRE_FALSE(menonRows.empty());
  const auto split = drogon::sync_wait(gallery.split(
      {.requester = ownerActor,
       .personId = barratt,
       .body = {.sampleIds = {menonRows.front()["id"].as<int64_t>()}}}));
  CHECK(split.visitor.id != barratt);
  CHECK(split.samples.size() == 1);
  CHECK(count("SELECT COUNT(*) AS n FROM person WHERE user_id IS NULL") == 3);

  drogon::sync_wait(gallery.remove({.requester = ownerActor, .personId = split.visitor.id}));
  CHECK(count("SELECT COUNT(*) AS n FROM person WHERE user_id IS NULL") == 2);
  CHECK(count("SELECT COUNT(*) AS n FROM face_embedding f LEFT JOIN person p ON "
              "p.id = f.person_id WHERE p.id IS NULL") == 0);
  CHECK(FaceService::instance().faceDb().count() ==
        static_cast<size_t>(count("SELECT COUNT(*) AS n FROM face_embedding")));

  CHECK_THROWS(drogon::sync_wait(
      gallery.remove({.requester = guardActor, .personId = barratt})));

  DbService::client()->execSqlSync(
      "UPDATE household_privacy SET visitor_recognition = 0 WHERE id = 1");
  CandidateRetentionService retention;
  CHECK(drogon::sync_wait(retention.sweep(t0 + 100000)) == 1);
  CHECK(count("SELECT COUNT(*) AS n FROM person WHERE user_id IS NULL AND "
              "deleted_at IS NULL") == 1);
  const auto ignored = observe(recognition, "barratt-b.jpg", t0 + 100100);
  CHECK(ignored.decision.outcome == SightingOutcome::Disabled);
}
