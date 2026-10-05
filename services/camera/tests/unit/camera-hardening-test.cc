#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/media/media-access-check.hxx>
#include <feature/operator/services/evidence/evidence-uploader.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <shared/repositories/tombstone-page.hxx>
#include <shared/services/secret-box/secret-box.hxx>
#include <shared/services/tapo/tapo-client.hxx>
#include <shared/services/tapo/tapo-trust.hxx>
#include <shared/vocabulary/operator-zone.hxx>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
constexpr std::array<uint8_t, secret_box::kKeyBytes> kKey{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

Json::Value tombstone(int64_t id, int64_t deletedAt)
{
  Json::Value row(Json::objectValue);
  row["id"] = Json::Int64(id);
  row["deletedAt"] = Json::Int64(deletedAt);
  return row;
}
}

TEST_CASE("camera secrets are sealed with the instance key and bound to their column")
{
  secret_box::clearKey();
  CHECK(secret_box::seal({.plain = "pw", .label = "camera.password"}) == "pw");
  REQUIRE(secret_box::installKey(kKey));
  const std::string sealed = secret_box::seal({.plain = "c0nfidential", .label = "camera.password"});
  CHECK(secret_box::isSealed(sealed));
  CHECK(sealed.find("c0nfidential") == std::string::npos);
  CHECK(sealed != secret_box::seal({.plain = "c0nfidential", .label = "camera.password"}));
  CHECK(secret_box::open({.stored = sealed, .label = "camera.password"}) == "c0nfidential");
  CHECK(secret_box::open({.stored = sealed, .label = "camera.cloud_password"}).empty());
  CHECK(secret_box::open({.stored = "legacy-plain", .label = "camera.password"}) == "legacy-plain");
  CHECK(secret_box::seal({.plain = "", .label = "camera.password"}).empty());

  std::string tampered = sealed;
  tampered[tampered.size() - 3] = tampered[tampered.size() - 3] == 'A' ? 'B' : 'A';
  CHECK(secret_box::open({.stored = tampered, .label = "camera.password"}).empty());
  secret_box::clearKey();
  CHECK(secret_box::open({.stored = sealed, .label = "camera.password"}).empty());
}

TEST_CASE("the instance key file is created once, private, and read back")
{
  const std::string path =
      (std::filesystem::temp_directory_path() / "camera-hardening-test.key").string();
  std::remove(path.c_str());
  secret_box::clearKey();
  CHECK(secret_box::loadOrCreateKey(path) == secret_box::KeyFileResult::Created);
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  CHECK((info.st_mode & 0777) == 0600);
  CHECK(info.st_size == static_cast<off_t>(secret_box::kKeyBytes));
  const std::string sealed = secret_box::seal({.plain = "x", .label = "l"});
  secret_box::clearKey();
  CHECK(secret_box::loadOrCreateKey(path) == secret_box::KeyFileResult::Loaded);
  CHECK(secret_box::open({.stored = sealed, .label = "l"}) == "x");
  secret_box::clearKey();
  std::remove(path.c_str());
}

TEST_CASE("a Tapo certificate is pinned on first use and a secure camera never falls back")
{
  std::vector<TapoTrustState> saved;
  TapoTrust trust({.fingerprint = {}, .secure = false},
                  [&saved](const TapoTrustState& state) { saved.push_back(state); });
  trust.learn("");
  CHECK(saved.empty());
  trust.learn("aa11");
  trust.learn("bb22");
  CHECK(trust.pin() == "aa11");
  trust.noteSecure();
  trust.noteSecure();
  REQUIRE(saved.size() == 2);
  CHECK(saved.back().fingerprint == "aa11");
  CHECK(saved.back().secure);

  TapoClientConfig config;
  config.host = "192.0.2.1";
  config.transport = TapoTransportPreference::LegacyStok;
  config.candidates.push_back({.label = "camera_account", .username = "u", .password = "p"});
  config.trust = std::make_shared<TapoTrust>(TapoTrustState{.fingerprint = "aa11", .secure = true},
                                             TapoTrust::Persist{});
  TapoClient client(config);
  const TapoResult refused = client.connect();
  CHECK_FALSE(refused.ok);
  CHECK(refused.error.find("legacy login refused") != std::string::npos);
}

TEST_CASE("a tombstone page re-reads its boundary second and keeps each id once")
{
  SyncFilter filter;
  CHECK_FALSE(tombstone_page::rereadsBoundary(filter));
  filter.startTime = 100;
  filter.startId = 50;
  CHECK(tombstone_page::rereadsBoundary(filter));
  const auto rows = tombstone_page::merge(
      {.boundary = {tombstone(10, 100), tombstone(50, 100)},
       .page = {tombstone(50, 100), tombstone(51, 100), tombstone(7, 101)}});
  REQUIRE(rows.size() == 4);
  CHECK(rows[0]["id"].asInt64() == 10);
  CHECK(rows[1]["id"].asInt64() == 50);
  CHECK(rows[2]["id"].asInt64() == 51);
  CHECK(rows[3]["id"].asInt64() == 7);
}

TEST_CASE("camera rows reach guards and guests without their address or account")
{
  Json::Value row(Json::objectValue);
  row["ip"] = "192.168.1.30";
  row["port"] = 554;
  row["username"] = "admin";
  row["cloudUsername"] = "owner@example.com";
  row["config"] = R"({"streamPath":"/live"})";
  row["name"] = "Patio";
  for (const UserRole role : {UserRole::Guard, UserRole::Guest}) {
    Json::Value reduced = row;
    camera_sync_projection::apply(reduced, role);
    CHECK(reduced["ip"].asString().empty());
    CHECK(reduced["port"].asInt() == 0);
    CHECK(reduced["username"].asString().empty());
    CHECK(reduced["cloudUsername"].asString().empty());
    CHECK(reduced["config"].asString() == "{}");
    CHECK(reduced["name"].asString() == "Patio");
  }
  for (const UserRole role : {UserRole::Owner, UserRole::Resident}) {
    Json::Value full = row;
    camera_sync_projection::apply(full, role);
    CHECK(full == row);
  }
}

TEST_CASE("evidence is kept for the camera's retention, capped at 60 days or 120 with an incident")
{
  constexpr int64_t kDay = 24 * 3600;
  CHECK(EvidenceUploader::retentionSecondsOf({.retentionDays = std::nullopt, .incident = false}) ==
        7 * kDay);
  CHECK(EvidenceUploader::retentionSecondsOf({.retentionDays = 30, .incident = false}) == 30 * kDay);
  CHECK(EvidenceUploader::retentionSecondsOf({.retentionDays = 365, .incident = false}) == 60 * kDay);
  CHECK(EvidenceUploader::retentionSecondsOf({.retentionDays = 365, .incident = true}) == 120 * kDay);
  CHECK(EvidenceUploader::retentionSecondsOf({.retentionDays = 0, .incident = false}) == 0);
}

TEST_CASE("a media socket is closed when its session lapses or its role changes")
{
  const MediaCredential credential{.token = "t", .deviceHash = "d", .origin = "lan",
                                   .role = UserRole::Resident};
  CHECK(MediaAccessCheck::judge(credential, UserRole::Resident) == MediaAccessVerdict::Keep);
  CHECK(MediaAccessCheck::judge(credential, UserRole::Guest) == MediaAccessVerdict::RoleChanged);
  CHECK(MediaAccessCheck::judge(credential, std::nullopt) == MediaAccessVerdict::Expired);
}

TEST_CASE("a configured zone kind is parsed once and an unknown one is refused")
{
  CHECK(operator_zone::kindOf("alert") == ZoneType::Alert);
  CHECK(operator_zone::kindOf("privacy") == ZoneType::Privacy);
  CHECK_FALSE(operator_zone::kindOf("notify").has_value());
  CHECK_FALSE(operator_zone::kindOf("").has_value());
}
