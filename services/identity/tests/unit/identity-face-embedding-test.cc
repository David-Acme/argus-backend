#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>

#include <shared/repositories/face-embedding/face-embedding-repository.hxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <drogon/drogon.h>
#include <sqlite/db-service.hxx>
#include <string>
#include <thread>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{

constexpr const char* kDb = "identity-face-embedding-test.db";

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

}

TEST_CASE("a face embedding is stored whole, zero bytes included")
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
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  DbService::client()->execSqlSync(
      "INSERT INTO person (id, name) VALUES (3, 'Sam')");

  constexpr std::array<float, 4> kEmbedding{0.0F, 1.0F, -0.5F, 0.0F};
  std::string bytes(kEmbedding.size() * sizeof(float), '\0');
  std::memcpy(bytes.data(), kEmbedding.data(), bytes.size());

  const FaceEmbeddingRepository repository;
  const FaceEmbeddingCreateInput input{.personId = 3,
                                       .embedding = bytes,
                                       .angleLabel = "frontal",
                                       .quality = 1.0,
                                       .client = nullptr};
  const auto created = drogon::sync_wait(repository.create(input));
  CHECK(created.id > 0);

  const auto stored = drogon::sync_wait(repository.findByPerson(3));
  REQUIRE(stored.size() == 1);
  CHECK(stored.front().embedding.size() == bytes.size());
  CHECK(stored.front().embedding == bytes);

  const auto kind = DbService::client()->execSqlSync(
      "SELECT typeof(embedding) AS kind FROM face_embedding");
  CHECK(kind.front()["kind"].as<std::string>() == "blob");
}
