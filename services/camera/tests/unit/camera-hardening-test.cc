#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/camera-row-projection.hxx>
#include <drogon/orm/DbClient.h>
#include <feature/actions/repositories/action-command/action-command-query.hxx>
#include <feature/media/media-access-check.hxx>
#include <feature/operator/services/evidence/evidence-query.hxx>
#include <feature/operator/services/evidence/evidence-uploader.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/tombstone-page.hxx>
#include <shared/services/secret-box/secret-box.hxx>
#include <shared/services/tapo/tapo-client.hxx>
#include <shared/services/tapo/tapo-trust.hxx>
#include <shared/vocabulary/operator-zone.hxx>
#include <sqlite/db-service.hxx>

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
constexpr std::array<uint8_t, secret_box::kKeyBytes> kKey{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

constexpr const char* kLegacyCameraTable =
    "CREATE TABLE camera ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "name TEXT NOT NULL, manufacturer TEXT NOT NULL DEFAULT '', "
    "model TEXT NOT NULL DEFAULT '', ip TEXT NOT NULL, "
    "port INTEGER NOT NULL DEFAULT 554, "
    "username TEXT NOT NULL DEFAULT 'admin', "
    "password TEXT NOT NULL DEFAULT '', "
    "cloud_username TEXT NOT NULL DEFAULT '', "
    "cloud_password TEXT NOT NULL DEFAULT '', "
    "driver TEXT NOT NULL DEFAULT 'tapo', "
    "icon TEXT NOT NULL DEFAULT 'video', "
    "record_mode TEXT NOT NULL DEFAULT 'events', "
    "retention_days INTEGER, capabilities TEXT NOT NULL DEFAULT '[]', "
    "config TEXT NOT NULL DEFAULT '{}', "
    "is_enabled INTEGER NOT NULL DEFAULT 1, "
    "is_online INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
    "updated_at INTEGER, deleted_at INTEGER)";

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    sqlite3_close_v2(raw);
    throw std::runtime_error("sqlite3_open " + path);
  }
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  const std::string message = error != nullptr ? error : "";
  sqlite3_free(error);
  if (rc != SQLITE_OK)
    throw std::runtime_error("sqlite3_exec: " + message);
}

class ClosingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char*, uint64_t, const drogon::WebSocketMessageType) override {}
  void send(std::string_view, const drogon::WebSocketMessageType) override {}
  void sendJson(const Json::Value&, const drogon::WebSocketMessageType) override {}
  [[nodiscard]] const trantor::InetAddress& localAddr() const override { return address_; }
  [[nodiscard]] const trantor::InetAddress& peerAddr() const override { return address_; }
  [[nodiscard]] bool connected() const override { return closedWith.empty(); }
  [[nodiscard]] bool disconnected() const override { return !closedWith.empty(); }
  void shutdown(const drogon::CloseCode, const std::string& reason) override { closedWith = reason; }
  void forceClose() override { closedWith = "forced"; }
  void setPingMessage(const std::string&, const std::chrono::duration<double>&) override {}
  void disablePing() override {}

  std::string closedWith;

private:
  trantor::InetAddress address_{"127.0.0.1", 0};
};

struct Tombstone
{
  int64_t id{0};
  int64_t deletedAt{0};
};

Json::Value tombstone(const Tombstone& input)
{
  const auto [id, deletedAt] = input;
  Json::Value row(Json::objectValue);
  row["id"] = static_cast<Json::Int64>(id);
  row["deletedAt"] = static_cast<Json::Int64>(deletedAt);
  return row;
}
}

TEST_CASE("camera secrets are sealed with the instance key and bound to their column")
{
  secret_box::clearKey();
  CHECK(secret_box::seal({.plain = "pw", .label = "camera.password"}) == "pw");
  REQUIRE(secret_box::installKey(kKey));
  const std::string sealed =
      secret_box::seal({.plain = "c0nfidential", .label = "camera.password"}).value_or("");
  CHECK(secret_box::isSealed(sealed));
  CHECK(sealed.find("c0nfidential") == std::string::npos);
  CHECK(sealed != secret_box::seal({.plain = "c0nfidential", .label = "camera.password"}));
  CHECK(secret_box::open({.stored = sealed, .label = "camera.password"}) == "c0nfidential");
  CHECK_FALSE(secret_box::open({.stored = sealed, .label = "camera.cloud_password"}).has_value());
  CHECK(secret_box::open({.stored = "legacy-plain", .label = "camera.password"}) == "legacy-plain");
  CHECK(secret_box::seal({.plain = "", .label = "camera.password"}) == "");

  std::string tampered = sealed;
  const std::size_t inner = secret_box::kPrefix.size() + 8;
  tampered[inner] = tampered[inner] == 'A' ? 'B' : 'A';
  CHECK_FALSE(secret_box::open({.stored = tampered, .label = "camera.password"}).has_value());
  secret_box::clearKey();
  CHECK_FALSE(secret_box::open({.stored = sealed, .label = "camera.password"}).has_value());
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
  CHECK((info.st_mode & 0777U) == 0600U);
  CHECK(info.st_size == static_cast<off_t>(secret_box::kKeyBytes));
  const std::string sealed = secret_box::seal({.plain = "x", .label = "l"}).value_or("");
  secret_box::clearKey();
  CHECK(secret_box::loadOrCreateKey(path) == secret_box::KeyFileResult::Loaded);
  CHECK(secret_box::open({.stored = sealed, .label = "l"}) == "x");
  secret_box::clearKey();
  std::remove(path.c_str());
}

TEST_CASE("an upgraded camera.db keeps every password, sealed in place, and refuses a foreign key")
{
  const std::string path =
      (std::filesystem::temp_directory_path() / "camera-hardening-secrets.db").string();
  std::remove(path.c_str());
  {
    const auto db = openFile(path);
    exec(db.get(), kLegacyCameraTable);
    exec(db.get(),
         "INSERT INTO camera (id, name, ip, password, cloud_password) VALUES "
         "(1, 'Patio', '192.168.1.30', 'rtsp-pass', 'cloud-pass'), "
         "(2, 'Sala', '192.168.1.31', '', ''), "
         "(3, 'Garaje', '192.168.1.32', 'only-rtsp', '')");
  }
  const auto client = drogon::orm::DbClient::newSqlite3Client("filename=" + path, 1);
  DbService::setCameraClient(client);

  secret_box::clearKey();
  REQUIRE(secret_box::installKey(kKey));
  CHECK(CameraRepository::acceptTapoTrust());
  CHECK(CameraRepository::sealPlaintextSecrets() == 2);
  CHECK(CameraRepository::sealPlaintextSecrets() == 0);
  CHECK(CameraRepository::unreadableSecrets() == 0);

  const auto rows = client->execSqlSync("SELECT * FROM camera ORDER BY id");
  REQUIRE(rows.size() == 3);
  CHECK(secret_box::isSealed(rows[0]["password"].as<std::string>()));
  CHECK(secret_box::isSealed(rows[0]["cloud_password"].as<std::string>()));
  CHECK(rows[1]["password"].as<std::string>().empty());
  CHECK(rows[2]["cloud_password"].as<std::string>().empty());
  CHECK(CameraSchema(rows[0]).password == "rtsp-pass");
  CHECK(CameraSchema(rows[0]).cloudPassword == "cloud-pass");
  CHECK(CameraSchema(rows[2]).password == "only-rtsp");
  CHECK(CameraSchema(rows[0]).toJson().isMember("ip"));
  CHECK_FALSE(CameraSchema(rows[0]).toJson().isMember("password"));
  CHECK_FALSE(CameraSchema(rows[0]).toJson().isMember("tlsFingerprint"));

  std::array<uint8_t, secret_box::kKeyBytes> foreign = kKey;
  foreign[0] = 99;
  REQUIRE(secret_box::installKey(foreign));
  CHECK(CameraRepository::unreadableSecrets() == 2);
  CHECK(CameraRepository::sealPlaintextSecrets() == 0);
  const auto untouched = client->execSqlSync("SELECT password FROM camera WHERE id = 1");
  CHECK(untouched.front()["password"].as<std::string>() == rows[0]["password"].as<std::string>());

  client->execSqlSync(std::string(camera_query::SAVE_TAPO_TRUST), std::string("aa11"), 1,
                      int64_t{1}, std::string("192.168.1.30"), std::string("aa11"));
  client->execSqlSync(std::string(camera_query::SAVE_TAPO_TRUST), std::string("ff99"), 1,
                      int64_t{1}, std::string("192.168.1.30"), std::string("ff99"));
  client->execSqlSync(std::string(camera_query::SAVE_TAPO_TRUST), std::string("ee77"), 1,
                      int64_t{2}, std::string("192.168.1.99"), std::string("ee77"));
  const auto pins = client->execSqlSync("SELECT id, tls_fingerprint FROM camera ORDER BY id");
  CHECK(pins[0]["tls_fingerprint"].as<std::string>() == "aa11");
  CHECK(pins[1]["tls_fingerprint"].as<std::string>().empty());

  secret_box::clearKey();
  DbService::setCameraClient(nullptr);
  std::remove(path.c_str());
}

TEST_CASE("expired evidence follows the camera's current retention, and settled commands are purged")
{
  constexpr int64_t kDay = int64_t{24} * 3600;
  constexpr int64_t kNow = 400 * kDay;
  const std::string path =
      (std::filesystem::temp_directory_path() / "camera-hardening-retention.db").string();
  std::remove(path.c_str());
  const auto db = openFile(path);
  exec(db.get(), kLegacyCameraTable);
  exec(db.get(),
       "CREATE TABLE camera_evidence (id INTEGER PRIMARY KEY AUTOINCREMENT, "
       "camera_id INTEGER NOT NULL DEFAULT 0, object_key TEXT NOT NULL DEFAULT '', "
       "content_type TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL DEFAULT 0, "
       "expires_at INTEGER NOT NULL DEFAULT 0, deleted_at INTEGER NOT NULL DEFAULT 0)");
  exec(db.get(),
       "INSERT INTO camera (id, name, ip, retention_days, config) VALUES "
       "(1, 'short', '192.168.1.30', 7, '{}'), "
       "(2, 'incident', '192.168.1.31', 90, '{\"retentionIncident\":true}'), "
       "(3, 'legacy', '192.168.1.32', 3650, 'not json')");
  const auto insert = [&db](int64_t camera, int64_t ageDays, int64_t expiresInDays) {
    exec(db.get(), "INSERT INTO camera_evidence (camera_id, object_key, created_at, expires_at) "
                   "VALUES (" + std::to_string(camera) + ", 'k', " +
                       std::to_string(kNow - ageDays * kDay) + ", " +
                       std::to_string(kNow + expiresInDays * kDay) + ")");
  };
  insert(1, 10, 50);
  insert(1, 3, 50);
  insert(2, 80, 20);
  insert(2, 121, 20);
  insert(3, 61, 3000);
  insert(3, 59, 3000);
  insert(9, 30, 5);

  const std::string query(evidence_query::EXPIRED_EVIDENCE);
  sqlite3_stmt* raw = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.get(), query.c_str(), -1, &raw, nullptr) == SQLITE_OK);
  const std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> statement(raw, sqlite3_finalize);
  sqlite3_bind_int64(statement.get(), 1, 0);
  sqlite3_bind_int64(statement.get(), 2, kNow);
  sqlite3_bind_int64(statement.get(), 3, kNow - 120 * kDay);
  sqlite3_bind_int64(statement.get(), 4, kNow);
  std::vector<int64_t> expired;
  while (sqlite3_step(statement.get()) == SQLITE_ROW)
    expired.push_back(sqlite3_column_int64(statement.get(), 0));
  CHECK(expired == std::vector<int64_t>{1, 4, 5});

  exec(db.get(),
       "CREATE TABLE action_command (command_id TEXT PRIMARY KEY, "
       "status TEXT NOT NULL DEFAULT 'executing', response TEXT NOT NULL DEFAULT '', "
       "updated_at INTEGER NOT NULL DEFAULT 0)");
  exec(db.get(),
       "INSERT INTO action_command (command_id, status, response, updated_at) VALUES "
       "('old-done', 'succeeded', 'visitor said hi', 100), "
       "('old-running', 'executing', '', 100), "
       "('new-done', 'succeeded', 'recent', 900)");
  const std::string purge = std::string(action_command_query::PURGE_SETTLED);
  sqlite3_stmt* rawPurge = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.get(), purge.c_str(), -1, &rawPurge, nullptr) == SQLITE_OK);
  const std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> purgeStatement(rawPurge,
                                                                             sqlite3_finalize);
  sqlite3_bind_int64(purgeStatement.get(), 1, 500);
  CHECK(sqlite3_step(purgeStatement.get()) == SQLITE_DONE);
  CHECK(sqlite3_changes(db.get()) == 1);
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
  config.candidates.push_back({.label = "camera_account", .username = "u", .password = "p",
                               .memoryKey = ""});
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
      {.boundary = {tombstone({.id = 10, .deletedAt = 100}), tombstone({.id = 50, .deletedAt = 100})},
       .page = {tombstone({.id = 50, .deletedAt = 100}), tombstone({.id = 51, .deletedAt = 100}), tombstone({.id = 7, .deletedAt = 101})}});
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
    for (const std::string_view field : camera_projection::kConnectionFields)
      CHECK(reduced[std::string(field)] != row[std::string(field)]);
  }
  for (const UserRole role : {UserRole::Owner, UserRole::Resident}) {
    Json::Value full = row;
    camera_sync_projection::apply(full, role);
    CHECK(full == row);
  }
}

TEST_CASE("evidence is kept for the camera's retention, capped at 60 days or 120 with an incident")
{
  constexpr int64_t kDay = int64_t{24} * 3600;
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
                                   .userId = 7, .role = UserRole::Resident};
  CHECK(MediaAccessCheck::judge(credential, MediaIdentity{.userId = 7, .role = UserRole::Resident}) ==
        MediaAccessVerdict::Keep);
  CHECK(MediaAccessCheck::judge(credential, MediaIdentity{.userId = 7, .role = UserRole::Guest}) ==
        MediaAccessVerdict::RoleChanged);
  CHECK(MediaAccessCheck::judge(credential, MediaIdentity{.userId = 8, .role = UserRole::Resident}) ==
        MediaAccessVerdict::Expired);
  CHECK(MediaAccessCheck::judge(credential, std::nullopt) == MediaAccessVerdict::Expired);
}

TEST_CASE("a media socket renews its access in band and keeps every revocation")
{
  std::map<std::string, std::optional<MediaIdentity>> tokens{
      {"old", MediaIdentity{.userId = 7, .role = UserRole::Resident}},
      {"new", MediaIdentity{.userId = 7, .role = UserRole::Resident}},
      {"other-user", MediaIdentity{.userId = 9, .role = UserRole::Resident}}};
  std::mutex tokensMutex;
  std::vector<std::string> deviceHashes;
  MediaAccessCheck access([&](const MediaCredential& credential) -> std::optional<MediaIdentity> {
    std::scoped_lock lock(tokensMutex);
    deviceHashes.push_back(credential.deviceHash);
    const auto found = tokens.find(credential.token);
    return found == tokens.end() ? std::nullopt : found->second;
  });
  const auto expire = [&](const std::string& token) {
    std::scoped_lock lock(tokensMutex);
    tokens[token] = std::nullopt;
  };
  const MediaCredential credential{.token = "old", .deviceHash = "device-a", .origin = "lan",
                                   .userId = 7, .role = UserRole::Resident};
  const auto start = std::chrono::steady_clock::now();

  const auto renewing = std::make_shared<ClosingConnection>();
  access.add({.connection = renewing, .credential = credential});
  CHECK(access.renewNow({.connection = renewing, .token = "new", .at = start}) ==
        MediaRenewal::Renewed);
  CHECK(deviceHashes.back() == "device-a");
  CHECK(access.renewNow({.connection = renewing, .token = "new", .at = start + std::chrono::seconds(1)}) ==
        MediaRenewal::Throttled);
  expire("old");
  CHECK(access.sweepNow() == 0);
  CHECK(renewing->closedWith.empty());

  const auto silent = std::make_shared<ClosingConnection>();
  access.add({.connection = silent, .credential = credential});
  CHECK(access.sweepNow() == 1);
  CHECK(silent->closedWith == "session_expired");
  CHECK(renewing->closedWith.empty());

  const auto intruder = std::make_shared<ClosingConnection>();
  access.add({.connection = intruder,
              .credential = {.token = "new", .deviceHash = "device-b", .origin = "lan",
                             .userId = 7, .role = UserRole::Resident}});
  CHECK(access.renewNow({.connection = intruder, .token = "other-user", .at = start}) ==
        MediaRenewal::Closed);
  CHECK(intruder->closedWith == "session_expired");
  CHECK(access.renewNow({.connection = std::make_shared<ClosingConnection>(), .token = "new",
                         .at = start}) == MediaRenewal::Unknown);

  access.remove(silent);
  access.remove(intruder);
  expire("new");
  CHECK(access.sweepNow() == 1);
  CHECK(renewing->closedWith == "session_expired");
}

TEST_CASE("a configured zone kind is parsed once and an unknown one is refused")
{
  CHECK(operator_zone::kindOf("alert") == ZoneType::Alert);
  CHECK(operator_zone::kindOf("privacy") == ZoneType::Privacy);
  CHECK_FALSE(operator_zone::kindOf("notify").has_value());
  CHECK_FALSE(operator_zone::kindOf("").has_value());
}
