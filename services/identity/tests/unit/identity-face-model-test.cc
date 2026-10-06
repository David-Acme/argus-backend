#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <feature/face-upgrade/services/face-upgrade-service.hxx>
#include <shared/services/face/face-quality.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/vocabulary/face-model.hxx>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <optional>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <string>
#include <thread>
#include <vector>

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

constexpr const char* kDb = "identity-face-model-test.db";

std::string fixture(const std::string& name)
{
  std::ifstream in(std::string(ARGUS_TEST_FACE_FIXTURES) + "/" + name,
                   std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool modelsPresent()
{
  return std::filesystem::exists(std::string(ARGUS_TEST_FACE_MODELS) +
                                 "/recognizer.bin") &&
         std::filesystem::exists(std::string(ARGUS_TEST_FACE_MODELS) +
                                 "/detector.bin");
}

FaceService& loadedFaces()
{
  static const bool loaded = [] {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((std::string(kDb) + suffix).c_str());
    VecDb::instance().setDbFile(kDb);
    FaceService::instance().init(ARGUS_TEST_FACE_MODELS);
    return FaceService::instance().isLoaded();
  }();
  REQUIRE(loaded);
  return FaceService::instance();
}

template <typename T>
T present(const std::optional<T>& value)
{
  REQUIRE(value.has_value());
  return value.value_or(T{});
}

std::vector<float> embed(const std::string& name)
{
  return present(loadedFaces().analyzeImage({.imageBytes = fixture(name), .encodeFace = false}))
      .embedding;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b)
{
  return std::inner_product(a.begin(), a.end(), b.begin(), 0.0F);
}

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

template <typename T> T awaitOnLoop(drogon::Task<T> task)
{
  return drogon::sync_wait(std::move(task));
}

}

TEST_CASE("alignment samples the interleaved image at the landmark transform")
{
  constexpr int kWidth = 300;
  constexpr int kHeight = 300;
  constexpr float kScale = 2.0F;
  constexpr float kOffsetX = 20.0F;
  constexpr float kOffsetY = 10.0F;
  std::vector<uint8_t> image(static_cast<size_t>(kWidth) * kHeight * 3);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const size_t at = (static_cast<size_t>(y) * kWidth + x) * 3;
      image[at] = static_cast<uint8_t>(x % 256);
      image[at + 1] = static_cast<uint8_t>(y % 256);
      image[at + 2] = static_cast<uint8_t>((x + y) % 256);
    }
  constexpr std::array<float, 10> kReference = {
      30.2946F, 51.6963F, 65.5318F, 51.5014F, 48.0252F,
      71.7366F, 33.5493F, 92.3655F, 62.7299F, 92.2041F};
  std::array<float, 10> landmarks{};
  for (size_t i = 0; i < landmarks.size(); i += 2) {
    landmarks[i] = kReference[i] * kScale + kOffsetX;
    landmarks[i + 1] = kReference[i + 1] * kScale + kOffsetY;
  }

  const auto aligned = FaceService::alignFace({.rgbData = image.data(),
                                               .width = kWidth,
                                               .height = kHeight,
                                               .landmarks = landmarks.data()});
  REQUIRE(aligned.size() == static_cast<size_t>(FaceService::kAlignedSide) *
                                FaceService::kAlignedSide * 3);
  int mismatches = 0;
  for (int v = 0; v < FaceService::kAlignedSide; ++v)
    for (int u = 0; u < FaceService::kAlignedSide; ++u) {
      const int x = static_cast<int>(std::lround(static_cast<float>(u) * kScale + kOffsetX));
      const int y = static_cast<int>(std::lround(static_cast<float>(v) * kScale + kOffsetY));
      const size_t at = (static_cast<size_t>(v) * FaceService::kAlignedSide + u) * 3;
      const auto off = [](int a, int b) { return std::abs(a - b) > 1; };
      if (off(aligned[at], x % 256) || off(aligned[at + 1], y % 256) ||
          off(aligned[at + 2], (x + y) % 256))
        ++mismatches;
    }
  CHECK(mismatches == 0);
}

TEST_CASE("geometry reads a frontal face as frontal and a turned one as turned")
{
  const FaceQuality frontal = face_quality::geometry({.leftEyeX = 40,
                                                      .leftEyeY = 50,
                                                      .rightEyeX = 80,
                                                      .rightEyeY = 50,
                                                      .noseX = 60,
                                                      .noseY = 72,
                                                      .mouthLeftX = 45,
                                                      .mouthLeftY = 92,
                                                      .mouthRightX = 75,
                                                      .mouthRightY = 92});
  CHECK(frontal.interOcularPx == doctest::Approx(40.0F));
  CHECK(frontal.yaw == doctest::Approx(0.0F));
  CHECK(frontal.pitch == doctest::Approx(22.0F / 42.0F));
  const FaceQuality turned = face_quality::geometry({.leftEyeX = 40,
                                                     .leftEyeY = 50,
                                                     .rightEyeX = 70,
                                                     .rightEyeY = 50,
                                                     .noseX = 72,
                                                     .noseY = 72,
                                                     .mouthLeftX = 48,
                                                     .mouthLeftY = 92,
                                                     .mouthRightX = 70,
                                                     .mouthRightY = 92});
  CHECK(turned.yaw > 0.5F);
}

TEST_CASE("the recognizer separates people on the public-domain fixtures" *
          doctest::skip(!modelsPresent()))
{
  const auto barrattA = embed("barratt-a.jpg");
  const auto barrattB = embed("barratt-b.jpg");
  const auto meirA = embed("meir-a.jpg");
  const auto meirB = embed("meir-b.jpg");
  const auto hathaway = embed("hathaway.jpg");
  const auto menon = embed("menon.jpg");

  CHECK(cosine(barrattA, barrattB) > 0.80F);
  CHECK(cosine(meirA, meirB) > 0.60F);
  for (const auto* other : {&meirA, &meirB, &hathaway, &menon}) {
    CHECK(cosine(barrattA, *other) < 0.30F);
    CHECK(cosine(barrattB, *other) < 0.30F);
  }
  CHECK(cosine(meirA, hathaway) < 0.30F);
  CHECK(cosine(meirB, menon) < 0.30F);
  CHECK(cosine(hathaway, menon) < 0.30F);

  const auto crop = present(loadedFaces().analyzeImage(
      {.imageBytes = fixture("menon.jpg"), .encodeFace = true}));
  CHECK(crop.faces == 1);
  CHECK(crop.quality.detectorScore > 0.9F);
  CHECK(crop.quality.interOcularPx > 40.0F);
  CHECK(crop.quality.yaw < 0.2F);
  REQUIRE(crop.faceJpeg.size() > 2);
  CHECK(static_cast<unsigned char>(crop.faceJpeg[0]) == 0xFF);
  CHECK(static_cast<unsigned char>(crop.faceJpeg[1]) == 0xD8);
}

TEST_CASE("an account's legacy face is re-embedded from its portrait and the "
          "legacy rows leave the index" *
          doctest::skip(!modelsPresent()))
{
  loadedFaces();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                       .filename = kDb,
                                                       .name = "default",
                                                       .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot());
  DbService::installExtensions();
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));

  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(1, 'Mike', '', 'owner'), (2, 'Ann', '', 'resident')");
  client->execSqlSync("INSERT INTO person (id, user_id, name) VALUES "
                      "(10, 1, 'Mike'), (20, 2, 'Ann'), (30, NULL, '')");
  const std::vector<char> legacy(512, '\x01');
  for (const int64_t person : {10, 20, 30})
    client->execSqlSync("INSERT INTO face_embedding (person_id, embedding) "
                        "VALUES (?, ?)",
                        person, legacy);
  const std::vector<float> stale(128, 0.088F);
  for (const int64_t rowid : {1, 2, 3}) {
    std::scoped_lock lock(VecDb::instance().mutex());
    FaceEmbeddingRepository repository;
    REQUIRE(repository.insertVec(VecDb::instance().handle(),
                                 {.embedding = stale.data(),
                                  .dims = 128,
                                  .personId = rowid * 10,
                                  .faceEmbeddingId = rowid}));
  }

  FaceService::instance().faceDb().init();
  CHECK(FaceService::instance().faceDb().count() == 0);

  const FaceUpgradeService upgrade(
      [](int64_t userId) -> drogon::Task<std::optional<std::string>> {
        if (userId == 1)
          co_return fixture("barratt-a.jpg");
        co_return std::nullopt;
      });
  const auto report = awaitOnLoop(upgrade.run());
  CHECK(report.stale == 2);
  CHECK(report.upgraded == 1);
  CHECK(report.withoutPortrait == 1);
  CHECK(FaceService::instance().faceDb().count() == 1);

  const auto rows = client->execSqlSync(
      "SELECT person_id, model FROM face_embedding ORDER BY id");
  REQUIRE(rows.size() == 4);
  CHECK(rows[3]["person_id"].as<int64_t>() == 10);
  CHECK(rows[3]["model"].as<std::string>() == std::string(kFaceModelId));
  CHECK(rows[0]["model"].as<std::string>() == std::string(kLegacyFaceModelId));

  const auto again = awaitOnLoop(upgrade.run());
  CHECK(again.stale == 1);
  CHECK(again.upgraded == 0);

  const auto probe = embed("barratt-b.jpg");
  const auto match = present(FaceService::instance().faceDb().search(probe.data()));
  CHECK(match.first == 10);
  const auto stranger = embed("menon.jpg");
  CHECK_FALSE(FaceService::instance().faceDb().search(stranger.data()).has_value());
}
