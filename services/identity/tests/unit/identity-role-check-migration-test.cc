#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/role-storage/services/role-check-migration.hxx>

#include <sqlite3.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

DbHandle open(const std::string& path)
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  return {raw, sqlite3_close_v2};
}

bool run(sqlite3* db, const std::string& sql, std::string* error = nullptr)
{
  char* message = nullptr;
  const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK;
  if (error != nullptr && message != nullptr)
    *error = message;
  sqlite3_free(message);
  return ok;
}

void must(sqlite3* db, const std::string& sql)
{
  std::string error;
  INFO(sql);
  INFO(error);
  REQUIRE(run(db, sql, &error));
}

int64_t number(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  int64_t value = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

std::string text(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  std::string value;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    value = raw != nullptr ? raw : "";
  }
  sqlite3_finalize(stmt);
  return value;
}

std::string readFile(const std::string& path)
{
  std::ifstream file(path);
  REQUIRE(file.good());
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

void replaceOnce(std::string& haystack, const std::string& from, const std::string& to)
{
  const auto at = haystack.find(from);
  REQUIRE(at != std::string::npos);
  haystack.replace(at, from.size(), to);
}

std::string shippedSchema()
{
  return readFile(ARGUS_IDENTITY_SCHEMA);
}

std::string legacySchema()
{
  std::string schema = shippedSchema();
  replaceOnce(schema,
              "    role           TEXT    NOT NULL,",
              "    role           TEXT    NOT NULL  CHECK (role IN ('owner', 'resident', 'guard', 'guest')),");
  replaceOnce(schema,
              "    role              TEXT    NOT NULL,",
              "    role              TEXT    NOT NULL CHECK (role IN ('resident', 'guard', 'guest')),");
  return schema;
}

struct Scratch
{
  fs::path directory;

  Scratch()
      : directory(fs::temp_directory_path() /
                  ("argus-role-check-" + std::to_string(::getpid()) + "-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
  {
    fs::create_directories(directory);
  }
  ~Scratch()
  {
    std::error_code error;
    fs::remove_all(directory, error);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] std::string file(const std::string& name) const { return (directory / name).string(); }
};

constexpr std::array kDependants = {"person",
                                    "stored_file",
                                    "user_portrait",
                                    "portrait_preview_capability",
                                    "voice_profile",
                                    "voice_sample",
                                    "voice_device",
                                    "user_privacy",
                                    "invitation_redemption",
                                    "user_invitation",
                                    "user"};

void seed(sqlite3* db)
{
  must(db, "PRAGMA foreign_keys = ON");
  must(db,
       "INSERT INTO user (id, name, last_name, role) VALUES "
       "(1, 'Ana', 'Owner', 'owner'), (2, 'Beto', 'Resident', 'resident'), "
       "(3, 'Carla', 'Guard', 'guard'), (4, 'Dino', 'Guest', 'guest'), (5, 'Eva', 'Gone', 'guest')");
  must(db, "DELETE FROM user WHERE id = 5");
  must(db, "INSERT INTO person (id, user_id, name) VALUES (1, 2, 'Beto'), (2, NULL, 'Visitor')");
  must(db,
       "INSERT INTO stored_file (id, object_key, sha256, mime_type, byte_size, category, created_by) "
       "VALUES (1, 'portrait/2', 'abc', 'image/jpeg', 10, 'portrait', 2)");
  must(db, "INSERT INTO user_portrait (user_id, file_id) VALUES (2, 1)");
  must(db,
       "INSERT INTO portrait_preview_capability (token_hash, portrait_user_id, requester_user_id, expires_at) "
       "VALUES ('hash', 2, 1, 4102444800)");
  must(db,
       "INSERT INTO voice_profile (user_id, model, embedding, sample_count, speech_seconds, source, linked_at, "
       "refreshed_at) VALUES (2, 'eres2net', x'0102', 3, 12.5, 'passive', 1, 1)");
  must(db,
       "INSERT INTO voice_sample (user_id, model, device_hash, embedding, turns, speech_seconds, state, created_at) "
       "VALUES (2, 'eres2net', 'dev', x'0102', 2, 4.0, 'pending', 1)");
  must(db, "INSERT INTO voice_device (device_hash, user_id, last_call_at) VALUES ('dev', 2, 1)");
  must(db,
       "INSERT INTO user_privacy (user_id, notice_version, presence, face_cameras, voice_learning, camera_audio) "
       "VALUES (2, 1, 1, 0, 1, 0)");
  must(db,
       "INSERT INTO user_invitation (id, token_hash, role, max_redemptions, redemption_count, expires_at, "
       "created_by) VALUES (1, 'token-hash', 'guard', 1, 1, 4102444800, 1), "
       "(2, 'other-hash', 'guest', 1, 0, 4102444800, 1)");
  must(db, "INSERT INTO invitation_redemption (invitation_id, user_id) VALUES (1, 3)");
}

std::vector<int64_t> counts(sqlite3* db)
{
  std::vector<int64_t> list;
  list.reserve(std::size(kDependants));
  for (const auto* table : kDependants)
    list.push_back(number(db, std::string("SELECT COUNT(*) FROM \"") + table + "\""));
  return list;
}

void legacyDatabase(const std::string& path)
{
  const auto db = open(path);
  must(db.get(), legacySchema());
  seed(db.get());
}
}

TEST_CASE("the shipped schema carries no CHECK on a role and the legacy one does")
{
  const Scratch scratch;
  const auto fresh = open(scratch.file("fresh.db"));
  must(fresh.get(), shippedSchema());
  must(fresh.get(), "INSERT INTO user (name, last_name, role) VALUES ('Fut', 'Ure', 'agronomist')");
  must(fresh.get(), "INSERT INTO user_invitation (token_hash, role, max_redemptions, expires_at, created_by) "
                    "VALUES ('h', 'agronomist', 1, 4102444800, 1)");

  const auto legacy = open(scratch.file("legacy.db"));
  must(legacy.get(), legacySchema());
  CHECK_FALSE(run(legacy.get(), "INSERT INTO user (name, last_name, role) VALUES ('Fut', 'Ure', 'agronomist')"));
}

TEST_CASE("the rewrite drops the role CHECK and nothing else")
{
  const auto rewritten = role_check_migration::withoutRoleCheck(
      "CREATE TABLE IF NOT EXISTS user (\n    id INTEGER PRIMARY KEY,\n"
      "    role           TEXT    NOT NULL  CHECK (role IN ('owner', 'resident', 'guard', 'guest')),\n"
      "    lang           TEXT    NOT NULL  DEFAULT 'es'  CHECK (lang IN ('es', 'en'))\n)");
  REQUIRE(rewritten.has_value());
  const std::string text = rewritten.value_or("");
  CHECK(text.find("role           TEXT    NOT NULL,") != std::string::npos);
  CHECK(text.find("CHECK (lang IN ('es', 'en'))") != std::string::npos);
  CHECK(text.find("owner") == std::string::npos);

  const auto spaced = role_check_migration::withoutRoleCheck(
      "CREATE TABLE user (role TEXT NOT NULL check(  role   in ( 'a','b' ) ) , name TEXT)");
  REQUIRE(spaced.has_value());
  CHECK(spaced.value_or("") == "CREATE TABLE user (role TEXT NOT NULL , name TEXT)");

  CHECK_FALSE(role_check_migration::withoutRoleCheck("CREATE TABLE user (role TEXT NOT NULL)").has_value());
  CHECK_FALSE(role_check_migration::withoutRoleCheck(
                  "CREATE TABLE user (lang TEXT CHECK (lang IN ('es', 'en')))").has_value());
}

TEST_CASE("the backup path is a sibling of a file database and absent for memory and URI databases")
{
  CHECK(role_check_migration::backupPathFor("database/identity.db", 17) ==
        "database/identity.db.role-rebuild-17.bak");
  CHECK(role_check_migration::backupPathFor("", 17).empty());
  CHECK(role_check_migration::backupPathFor(":memory:", 17).empty());
  CHECK(role_check_migration::backupPathFor("file:x?mode=memory&cache=shared", 17).empty());
  CHECK(role_check_migration::backupPathFor("file:/data/identity.db?mode=rwc", 17).empty());
}

TEST_CASE("dropping the role CHECK keeps every row, reference, index and sequence and fires no cascade")
{
  const Scratch scratch;
  const std::string path = scratch.file("identity.db");
  legacyDatabase(path);
  const std::string backup = scratch.file("identity.db.bak");

  const auto db = open(path);
  must(db.get(), "PRAGMA foreign_keys = ON");
  const auto before = counts(db.get());
  REQUIRE(number(db.get(), "SELECT COUNT(*) FROM voice_profile") == 1);
  REQUIRE(number(db.get(), "SELECT COUNT(*) FROM user_portrait") == 1);
  REQUIRE(number(db.get(), "SELECT COUNT(*) FROM user_privacy") == 1);
  REQUIRE(number(db.get(), "SELECT COUNT(*) FROM person WHERE user_id = 2") == 1);
  REQUIRE(number(db.get(), "PRAGMA foreign_keys") == 1);

  const auto outcome = role_check_migration::apply({.db = db.get(), .backupPath = backup});
  INFO(outcome.error);
  REQUIRE(outcome.ok());
  CHECK(outcome.rebuilt == std::vector<std::string>{"user", "user_invitation"});
  CHECK(outcome.backup == backup);

  CHECK(counts(db.get()) == before);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM voice_profile WHERE user_id = 2") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM voice_sample WHERE user_id = 2") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM user_portrait WHERE user_id = 2") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM user_privacy WHERE user_id = 2") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM person WHERE user_id = 2") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM invitation_redemption WHERE user_id = 3") == 1);
  CHECK(text(db.get(), "SELECT name FROM user WHERE id = 3") == "Carla");
  CHECK(text(db.get(), "SELECT role FROM user_invitation WHERE id = 1") == "guard");
  CHECK(number(db.get(), "PRAGMA foreign_keys") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
  CHECK(text(db.get(), "PRAGMA integrity_check") == "ok");

  CHECK(text(db.get(), "SELECT sql FROM sqlite_master WHERE name = 'user'").find("role IN") == std::string::npos);
  CHECK(text(db.get(), "SELECT sql FROM sqlite_master WHERE name = 'user'").find("CHECK (lang IN") !=
        std::string::npos);
  CHECK(text(db.get(), "SELECT sql FROM sqlite_master WHERE name = 'user_invitation'").find("role IN") ==
        std::string::npos);
  CHECK(text(db.get(), "SELECT sql FROM sqlite_master WHERE name = 'user_invitation'").find("max_redemptions BETWEEN") !=
        std::string::npos);
  for (const auto* index : {"idx_user_created_at", "idx_user_deleted_at", "idx_user_invitation_token_hash",
                            "idx_user_invitation_active", "idx_user_invitation_creator"})
    CHECK(number(db.get(), std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = '") +
                               index + "'") == 1);

  must(db.get(), "INSERT INTO user (name, last_name, role) VALUES ('Fut', 'Ure', 'agronomist')");
  CHECK(number(db.get(), "SELECT id FROM user WHERE role = 'agronomist'") == 6);
  must(db.get(), "INSERT INTO user_invitation (token_hash, role, max_redemptions, expires_at, created_by) "
                 "VALUES ('h2', 'agronomist', 1, 4102444800, 1)");
  CHECK_FALSE(run(db.get(), "INSERT INTO user_invitation (token_hash, role, max_redemptions, expires_at, created_by) "
                            "VALUES ('h3', 'guest', 0, 4102444800, 1)"));

  must(db.get(), "DELETE FROM user WHERE id = 2");
  CHECK(number(db.get(), "SELECT COUNT(*) FROM voice_profile") == 0);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM user_portrait") == 0);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM user_privacy") == 0);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM person WHERE id = 1 AND user_id IS NULL") == 1);
  CHECK_FALSE(run(db.get(), "DELETE FROM user WHERE id = 1"));

  REQUIRE(fs::exists(backup));
  const auto saved = open(backup);
  CHECK(number(saved.get(), "SELECT COUNT(*) FROM user") == 4);
  CHECK(number(saved.get(), "SELECT COUNT(*) FROM voice_profile") == 1);
  CHECK(text(saved.get(), "SELECT sql FROM sqlite_master WHERE name = 'user'").find("role IN") != std::string::npos);
}

TEST_CASE("a second run finds nothing to do and takes no backup")
{
  const Scratch scratch;
  const std::string path = scratch.file("identity.db");
  legacyDatabase(path);
  const auto db = open(path);
  must(db.get(), "PRAGMA foreign_keys = ON");
  REQUIRE(role_check_migration::apply({.db = db.get(), .backupPath = scratch.file("first.bak")}).ok());

  const auto again = role_check_migration::apply({.db = db.get(), .backupPath = scratch.file("second.bak")});
  CHECK(again.ok());
  CHECK(again.rebuilt.empty());
  CHECK(again.backup.empty());
  CHECK_FALSE(fs::exists(scratch.file("second.bak")));
  CHECK(number(db.get(), "PRAGMA foreign_keys") == 1);
}

TEST_CASE("a database created from the shipped schema needs no rebuild")
{
  const Scratch scratch;
  const auto db = open(scratch.file("fresh.db"));
  must(db.get(), shippedSchema());
  const auto outcome = role_check_migration::apply({.db = db.get(), .backupPath = scratch.file("none.bak")});
  CHECK(outcome.ok());
  CHECK(outcome.rebuilt.empty());
  CHECK_FALSE(fs::exists(scratch.file("none.bak")));
}

TEST_CASE("a broken reference aborts the rebuild, changes nothing and puts the foreign keys back")
{
  const Scratch scratch;
  const std::string path = scratch.file("identity.db");
  legacyDatabase(path);
  const auto db = open(path);
  must(db.get(), "PRAGMA foreign_keys = OFF");
  must(db.get(),
       "INSERT INTO voice_sample (user_id, model, device_hash, embedding, turns, speech_seconds, state, created_at) "
       "VALUES (99, 'eres2net', 'dev', x'01', 1, 1.0, 'pending', 1)");
  must(db.get(), "PRAGMA foreign_keys = ON");
  const auto before = counts(db.get());

  const auto outcome = role_check_migration::apply({.db = db.get(), .backupPath = scratch.file("kept.bak")});
  CHECK_FALSE(outcome.ok());
  CHECK(outcome.error.find("foreign_key_check") != std::string::npos);
  CHECK(outcome.rebuilt.empty());
  CHECK(counts(db.get()) == before);
  CHECK(text(db.get(), "SELECT sql FROM sqlite_master WHERE name = 'user'").find("role IN") != std::string::npos);
  CHECK_FALSE(run(db.get(), "INSERT INTO user (name, last_name, role) VALUES ('Fut', 'Ure', 'agronomist')"));
  CHECK(number(db.get(), "PRAGMA foreign_keys") == 1);
  CHECK(number(db.get(), "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE '%__rebuild'") == 0);
}

TEST_CASE("the boot entry point migrates a file database in place and leaves a backup beside it")
{
  const Scratch scratch;
  const std::string path = scratch.file("identity.db");
  legacyDatabase(path);

  REQUIRE(role_check_migration::applyToFile(path));
  std::vector<std::string> backups;
  for (const auto& entry : fs::directory_iterator(scratch.directory))
    if (entry.path().filename().string().find(".role-rebuild-") != std::string::npos)
      backups.push_back(entry.path().string());
  REQUIRE(backups.size() == 1);

  const auto db = open(path);
  must(db.get(), "INSERT INTO user (name, last_name, role) VALUES ('Fut', 'Ure', 'agronomist')");
  CHECK(number(db.get(), "SELECT COUNT(*) FROM user") == 5);
  CHECK(number(open(backups.front()).get(), "SELECT COUNT(*) FROM user") == 4);

  CHECK(role_check_migration::applyToFile(path));
  std::size_t after = 0;
  for (const auto& entry : fs::directory_iterator(scratch.directory))
    if (entry.path().filename().string().find(".role-rebuild-") != std::string::npos)
      ++after;
  CHECK(after == 1);

  CHECK_FALSE(role_check_migration::applyToFile(scratch.file("missing/none.db")));
}
