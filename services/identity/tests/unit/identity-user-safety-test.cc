#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/enrollment/repositories/enrollment/enrollment-repository.hxx>
#include <feature/invitation/services/invitation-feature-service.hxx>
#include <feature/person/services/person-feature-service.hxx>
#include <feature/user/services/biometric-erase-service.hxx>
#include <feature/user/services/portrait-preview-service.hxx>
#include <feature/user/services/user-feature-service.hxx>
#include <feature/visitor/repositories/crop-capability/crop-capability-repository.hxx>
#include <feature/visitor/repositories/visitor/visitor-repository.hxx>
#include <identity/identity-errors.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sqlite/vec-db.hxx>
#include <sync/identity-change-sink.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kDb = "identity-user-safety-test.db";

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

std::unique_ptr<AppRunner>& runnerSlot()
{
  static std::unique_ptr<AppRunner> slot;
  return slot;
}

bool waitForBoot()
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

void boot()
{
  if (runnerSlot() == nullptr) {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((std::string(kDb) + suffix).c_str());
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{
        .connectionNumber = 1, .filename = kDb, .name = "default", .timeout = -1});
    VecDb::instance().setDbFile(kDb);
    runnerSlot() = std::make_unique<AppRunner>();
  }
  REQUIRE(waitForBoot());
  static const bool schema = [] {
    DbService::installExtensions();
    return DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA);
  }();
  REQUIRE(schema);
}

int64_t scalar(std::string_view sql)
{
  const auto rows = DbService::client()->execSqlSync(std::string(sql));
  return rows.empty() ? -1 : rows.front()[0].as<int64_t>();
}

template <typename T>
std::string refusalOf(drogon::Task<T> task)
{
  try {
    static_cast<void>(drogon::sync_wait(std::move(task)));
  }
  catch (const ResponseException& error) {
    return error.what();
  }
  return {};
}

class RecordingSink : public IdentityChangeSink
{
public:
  [[nodiscard]] drogon::Task<void> publishCatalog(const IdentityCatalogInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void> emitModule(const ModuleEmitInput&) const override
  {
    ++moduleFrames_;
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override
  {
    moduleAudits_.push_back(input.recordId);
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishUsersAudit(const UserAuditInput&) const override
  {
    co_return;
  }

  [[nodiscard]] drogon::Task<void> publishAction(const ActionPublishInput& input) const override
  {
    actions_.push_back(input.event.recordId);
    co_return;
  }

  [[nodiscard]] const std::vector<int64_t>& moduleAudits() const { return moduleAudits_; }
  [[nodiscard]] const std::vector<int64_t>& actions() const { return actions_; }
  [[nodiscard]] int moduleFrames() const { return moduleFrames_; }

private:
  mutable std::vector<int64_t> moduleAudits_;
  mutable std::vector<int64_t> actions_;
  mutable int moduleFrames_{0};
};

UpdateUserDto roleChange(UserRole role)
{
  UpdateUserDto body;
  body.role = userRoleToString(role);
  body.userRole = role;
  return body;
}
}

TEST_CASE("the last active owner can be neither demoted nor deactivated, and nobody "
          "deactivates their own account")
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(1, 'Olga', '', 'owner'), (2, 'Raul', '', 'resident')");
  const UserFeatureService users;

  CHECK(refusalOf(users.update({.targetUserId = 1,
                                .actorId = 1,
                                .body = roleChange(UserRole::Resident)})) ==
        IdentityErrors::ActiveOwnerRequired.message);
  UpdateUserDto off;
  off.isActive = false;
  CHECK(refusalOf(users.update({.targetUserId = 1, .actorId = 1, .body = off})) ==
        IdentityErrors::SelfDeactivationForbidden.message);
  CHECK(refusalOf(users.deactivate(1, 1)) ==
        IdentityErrors::SelfDeactivationForbidden.message);
  CHECK(refusalOf(users.update({.targetUserId = 1, .actorId = 2, .body = off})) ==
        IdentityErrors::ActiveOwnerRequired.message);
  CHECK(scalar("SELECT COUNT(*) FROM user WHERE role = 'owner' AND is_active = 1") == 1);

  const UserRepository repository;
  const auto guarded = drogon::sync_wait(repository.update(
      1, {.name = std::nullopt,
          .lastName = std::nullopt,
          .role = UserRole::Guest,
          .isActive = std::nullopt,
          .requireOtherActiveOwner = true,
          .client = nullptr}));
  CHECK(guarded.id == 0);
  CHECK(scalar("SELECT COUNT(*) FROM user WHERE id = 1 AND role = 'owner'") == 1);

  const auto promoted = drogon::sync_wait(users.update(
      {.targetUserId = 2, .actorId = 1, .body = roleChange(UserRole::Owner)}));
  CHECK(promoted.role == UserRole::Owner);
  const auto demoted = drogon::sync_wait(users.update(
      {.targetUserId = 1, .actorId = 2, .body = roleChange(UserRole::Resident)}));
  CHECK(demoted.role == UserRole::Resident);
  CHECK(refusalOf(users.update({.targetUserId = 2,
                                .actorId = 1,
                                .body = roleChange(UserRole::Guard)})) ==
        IdentityErrors::ActiveOwnerRequired.message);

  const auto renamed = drogon::sync_wait(users.rename({.userId = 1, .name = "Olga M."}));
  REQUIRE(renamed.has_value());
  CHECK(renamed.value_or(UserSchema{}).name == "Olga M.");
  CHECK_FALSE(drogon::sync_wait(users.rename({.userId = 99, .name = "Nobody"})).has_value());
}

TEST_CASE("a portrait preview and a visitor crop are seen once")
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(11, 'Gaby', '', 'guard'), (12, 'Pia', '', 'resident')");
  client->execSqlSync("INSERT INTO stored_file (id, object_key, sha256, mime_type, "
                      "byte_size, category, created_by) VALUES "
                      "(110, 'portraits/12/a.jpg', 'x', 'image/jpeg', 10, 'portrait', 12)");
  client->execSqlSync("INSERT INTO user_portrait (user_id, file_id) VALUES (12, 110)");

  const PortraitPreviewService previews;
  const auto capability = drogon::sync_wait(previews.create(
      {.portraitUserId = 12, .requesterUserId = 11, .requesterRole = UserRole::Guard}));
  CHECK(refusalOf(previews.create({.portraitUserId = 12,
                                   .requesterUserId = 12,
                                   .requesterRole = UserRole::Resident})) ==
        IdentityErrors::PortraitVerificationUnavailable.message);
  CHECK(refusalOf(previews.consume({.token = capability.token,
                                    .requesterUserId = 12,
                                    .requesterRole = UserRole::Owner})) ==
        IdentityErrors::PortraitPreviewUnavailable.message);
  CHECK_THROWS(drogon::sync_wait(previews.consume(
      {.token = capability.token, .requesterUserId = 11, .requesterRole = UserRole::Guard})));
  CHECK(scalar("SELECT COUNT(*) FROM portrait_preview_capability "
               "WHERE consumed_at IS NOT NULL") == 1);
  CHECK(refusalOf(previews.consume({.token = capability.token,
                                    .requesterUserId = 11,
                                    .requesterRole = UserRole::Guard})) ==
        IdentityErrors::PortraitPreviewUnavailable.message);
  static_cast<void>(drogon::sync_wait(previews.create(
      {.portraitUserId = 12, .requesterUserId = 11, .requesterRole = UserRole::Guard})));
  CHECK(scalar("SELECT COUNT(*) FROM portrait_preview_capability") == 1);

  client->execSqlSync("INSERT INTO person (id, user_id, name) VALUES (120, NULL, 'Juan')");
  client->execSqlSync("INSERT INTO face_embedding (id, person_id, embedding) "
                      "VALUES (1200, 120, x'00')");
  const CropCapabilityRepository crops;
  const int64_t now = std::time(nullptr);
  drogon::sync_wait(crops.create({.tokenHash = "crop-hash",
                                  .personId = 120,
                                  .sampleId = 1200,
                                  .requesterUserId = 11,
                                  .expiresAt = now + 60}));
  CHECK_FALSE(drogon::sync_wait(crops.consume({.tokenHash = "crop-hash",
                                               .requesterUserId = 12,
                                               .now = now,
                                               .client = nullptr}))
                  .has_value());
  CHECK(drogon::sync_wait(crops.consume({.tokenHash = "crop-hash",
                                         .requesterUserId = 11,
                                         .now = now,
                                         .client = nullptr}))
            .has_value());
  CHECK_FALSE(drogon::sync_wait(crops.consume({.tokenHash = "crop-hash",
                                               .requesterUserId = 11,
                                               .now = now,
                                               .client = nullptr}))
                  .has_value());
}

TEST_CASE("an invitation is consumed once")
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(21, 'Ines', '', 'owner')");
  const int64_t now = std::time(nullptr);
  client->execSqlSync("INSERT INTO user_invitation (token_hash, role, max_redemptions, "
                      "expires_at, created_by) VALUES ('invite-hash', 'guest', 1, ?, 21)",
                      now + 600);
  const EnrollmentRepository enrollment;
  CHECK(drogon::sync_wait(enrollment.consumeInvitation(
      {.tokenHash = "invite-hash", .now = now, .client = nullptr})));
  CHECK_FALSE(drogon::sync_wait(enrollment.consumeInvitation(
      {.tokenHash = "invite-hash", .now = now, .client = nullptr})));
  CHECK_FALSE(drogon::sync_wait(enrollment.consumeInvitation(
      {.tokenHash = "unknown-hash", .now = now, .client = nullptr})));
}

TEST_CASE("an owner erases a person's biometrics in one transaction and the objects "
          "wait in the deletion queue")
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(31, 'Omar', '', 'owner'), (32, 'Lia', '', 'resident')");
  client->execSqlSync("INSERT INTO person (id, user_id, name) VALUES (320, 32, 'Lia')");
  client->execSqlSync("INSERT INTO face_embedding (id, person_id, embedding, model, crop_key) "
                      "VALUES (3200, 320, x'00', 'legacy', ''), "
                      "(3201, 320, x'00', 'mobilefacenet-aligned-v2', 'faces/320/3201.jpg')");
  client->execSqlSync("INSERT INTO stored_file (id, object_key, sha256, mime_type, "
                      "byte_size, category, created_by) VALUES "
                      "(330, 'portraits/32/old.jpg', 'x', 'image/jpeg', 10, 'portrait', 32), "
                      "(331, 'portraits/32/new.jpg', 'y', 'image/jpeg', 10, 'portrait', 32)");
  client->execSqlSync("INSERT INTO user_portrait (user_id, file_id) VALUES (32, 331)");
  client->execSqlSync("INSERT INTO portrait_preview_capability (token_hash, "
                      "portrait_user_id, requester_user_id, expires_at) "
                      "VALUES ('p-hash', 32, 31, 9999999999)");
  client->execSqlSync("INSERT INTO voice_profile (user_id, model, embedding, sample_count, "
                      "speech_seconds, source, linked_at, refreshed_at) "
                      "VALUES (32, 'm', x'00', 1, 3.0, 'passive', 1, 1)");
  client->execSqlSync("INSERT INTO voice_sample (user_id, model, device_hash, embedding, "
                      "turns, speech_seconds, state, created_at) "
                      "VALUES (32, 'm', 'd', x'00', 2, 5.0, 'adopted', 1)");
  client->execSqlSync("INSERT INTO voice_device (device_hash, user_id, last_call_at) "
                      "VALUES ('d', 32, 1)");
  client->execSqlSync("INSERT INTO user_privacy (user_id, notice_version, presence, "
                      "face_cameras, voice_learning, camera_audio) VALUES (32, 1, 1, 1, 1, 1)");

  const BiometricEraseService erase;
  CHECK(refusalOf(erase.erase({.actorId = 32, .actorRole = UserRole::Resident, .subjectId = 32})) ==
        IdentityErrors::BiometricEraseOwnerOnly.message);
  CHECK(refusalOf(erase.erase({.actorId = 31, .actorRole = UserRole::Owner, .subjectId = 999})) ==
        IdentityErrors::UserNotFound.message);

  const auto erased = drogon::sync_wait(
      erase.erase({.actorId = 31, .actorRole = UserRole::Owner, .subjectId = 32}));
  CHECK(erased.faces == 2);
  CHECK(erased.portraits == 2);
  CHECK(erased.voiceProfile);
  CHECK(erased.voiceSamples == 1);
  CHECK(scalar("SELECT COUNT(*) FROM face_embedding WHERE person_id = 320") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM user_portrait WHERE user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM stored_file WHERE created_by = 32 "
               "AND deleted_at IS NULL") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM portrait_preview_capability "
               "WHERE portrait_user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM voice_profile WHERE user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM voice_sample WHERE user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM voice_device WHERE user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM user_privacy WHERE user_id = 32") == 0);
  CHECK(scalar("SELECT COUNT(*) FROM pending_object_delete WHERE object_key IN "
               "('faces/320/3201.jpg', 'portraits/32/old.jpg', "
               "'portraits/32/new.jpg')") == 3);
  CHECK(scalar("SELECT COUNT(*) FROM user WHERE id = 32 AND deleted_at IS NULL") == 1);
  CHECK(scalar("SELECT COUNT(*) FROM person WHERE id = 320 AND deleted_at IS NULL") == 1);
}

TEST_CASE("visitor numbers are never reused and visitor tombstones never reach sync")
{
  boot();
  const auto client = DbService::client();
  const VisitorRepository visitors;
  const auto createOne = [&visitors]() -> drogon::Task<int64_t> {
    auto transaction = co_await db_transaction::begin(DbService::identityClient());
    const int64_t id = co_await visitors.createVisitor(transaction.get());
    co_await db_transaction::Commit(std::move(transaction));
    co_return id;
  };
  const auto numberOf = [&client](int64_t id) {
    return client->execSqlSync("SELECT visitor_number FROM person WHERE id = ?", id)
        .front()["visitor_number"]
        .as<int64_t>();
  };
  const int64_t first = drogon::sync_wait(createOne());
  const int64_t second = drogon::sync_wait(createOne());
  CHECK(numberOf(second) == numberOf(first) + 1);
  client->execSqlSync("DELETE FROM person WHERE id = ?", second);
  const int64_t third = drogon::sync_wait(createOne());
  CHECK(numberOf(third) == numberOf(first) + 2);

  const int64_t now = std::time(nullptr);
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(41, 'Teo', '', 'guest')");
  client->execSqlSync("INSERT INTO person (id, user_id, name, deleted_at) VALUES "
                      "(410, 41, 'Teo', ?), (411, 41, 'Teo', ?), (412, NULL, '', ?)",
                      now - 30, now, now - 30);
  const PersonRepository persons;
  const auto tombstones = drogon::sync_wait(persons.findDeleted(SyncFilter{}));
  std::vector<int64_t> ids;
  for (const auto& row : tombstones)
    ids.push_back(row["id"].asInt64());
  CHECK(std::ranges::find(ids, 410) != ids.end());
  CHECK(std::ranges::find(ids, 411) == ids.end());
  CHECK(std::ranges::find(ids, 412) == ids.end());
}

TEST_CASE("promoting a visitor journals it for the owner and never reaches the module rooms")
{
  boot();
  const auto client = DbService::client();
  client->execSqlSync("INSERT INTO user (id, name, last_name, role) VALUES "
                      "(51, 'Rosa', '', 'resident')");
  client->execSqlSync("INSERT INTO person (id, user_id, name, status) VALUES "
                      "(510, 51, 'Rosa', 'candidate'), (511, NULL, 'Cartero', 'candidate'), "
                      "(512, NULL, '', 'candidate')");
  const RecordingSink sink;
  identity_change::setSink(&sink);
  const PersonFeatureService persons;

  CHECK(drogon::sync_wait(persons.promote({.personId = 511, .actorId = 31})) == true);
  CHECK(drogon::sync_wait(persons.promote({.personId = 512, .actorId = 31})) == false);
  CHECK(drogon::sync_wait(persons.promote({.personId = 510, .actorId = 31})) == true);
  identity_change::setSink(nullptr);

  CHECK(sink.moduleAudits() == std::vector<int64_t>{510});
  CHECK(sink.actions() == std::vector<int64_t>{511});
  CHECK(sink.moduleFrames() == 0);
  CHECK(scalar("SELECT COUNT(*) FROM person WHERE id = 512 AND status = 'candidate'") == 1);
}

TEST_CASE("the user directory is read by an active guard and shrinks to its own row when its module is off")
{
  boot();
  DbService::client()->execSqlSync("INSERT OR IGNORE INTO user (id, name, last_name, role) VALUES "
                                   "(31, 'Gus', '', 'guard'), (32, 'Rita', '', 'resident'), "
                                   "(33, 'Odd', '', 'agronomist')");
  const UserFeatureService users;

  moduleGate().reset();
  CHECK(drogon::sync_wait(users.list(31, UserRole::Guard)).size() >= 3);
  CHECK(drogon::sync_wait(users.list(32, UserRole::Resident)).size() == 1);

  moduleGate().apply({{.id = "surveillance", .enabled = false, .lifecycle = "disabled", .dataPurgedAt = 0,
                       .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});
  const auto own = drogon::sync_wait(users.list(31, UserRole::Guard));
  REQUIRE(own.size() == 1);
  CHECK(own.front().id == 31);
  CHECK(drogon::sync_wait(users.list(32, UserRole::Resident)).size() == 1);
  const PortraitPreviewService previews;
  CHECK(refusalOf(previews.create({.portraitUserId = 32, .requesterUserId = 31, .requesterRole = UserRole::Guard})) ==
        IdentityErrors::PortraitVerificationUnavailable.message);

  moduleGate().apply({{.id = "surveillance", .enabled = true, .lifecycle = "active", .dataPurgedAt = 0,
                       .roles = {"guard"}, .name = {}, .summary = {}, .intro = {}}});
  CHECK(drogon::sync_wait(users.list(31, UserRole::Guard)).size() >= 3);

  const auto stored = drogon::sync_wait(UserRepository{}.findById(33));
  REQUIRE(stored.has_value());
  CHECK(stored.value_or(UserSchema{}).role == UserRole::Unknown);
  CHECK(drogon::sync_wait(users.list(33, UserRole::Unknown)).size() == 1);
  moduleGate().reset();
}

namespace
{
ModuleFlag surveillance(bool enabled)
{
  return {.id = "surveillance",
          .enabled = enabled,
          .lifecycle = enabled ? "active" : "disabled",
          .dataPurgedAt = 0,
          .roles = {"guard"},
          .name = {.es = "Vigilancia", .en = "Surveillance"},
          .summary = {},
          .intro = {}};
}

struct RefusalDetail
{
  int status{0};
  std::string code;
  std::string message;
};

template <typename T>
RefusalDetail detailOf(drogon::Task<T> task)
{
  try {
    static_cast<void>(drogon::sync_wait(std::move(task)));
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(), .code = error.errorCode(), .message = error.what()};
  }
  return {};
}
}

TEST_CASE("an invitation for a role whose module is off is refused 409 ROLE_INACTIVE, naming the module in the Owner's language")
{
  boot();
  DbService::client()->execSqlSync(
      "INSERT OR IGNORE INTO user (id, name, last_name, role, lang) VALUES "
      "(61, 'Olivia', '', 'owner', 'en'), (62, 'Oscar', '', 'owner', 'es')");
  const InvitationFeatureService invitations;
  const auto ask = [](UserRole role) {
    CreateInvitationDto body;
    body.role = userRoleToString(role);
    body.userRole = role;
    return body;
  };

  moduleGate().reset();
  moduleGate().apply({surveillance(false)});

  const auto english = detailOf(invitations.create(ask(UserRole::Guard), 61));
  CHECK(english.status == 409);
  CHECK(english.code == "ROLE_INACTIVE");
  CHECK(english.message == "Guard needs the Surveillance module");
  const auto spanish = detailOf(invitations.create(ask(UserRole::Guard), 62));
  CHECK(spanish.status == 409);
  CHECK(spanish.message == "Vigilante necesita el módulo Vigilancia");
  CHECK(scalar("SELECT COUNT(*) FROM user_invitation WHERE role = 'guard'") == 0);

  const auto resident = drogon::sync_wait(invitations.create(ask(UserRole::Resident), 61));
  CHECK(resident.invitation.role == UserRole::Resident);
  CHECK(detailOf(invitations.create(ask(UserRole::Owner), 61)).status == 422);

  moduleGate().apply({surveillance(true)});
  const auto guard = drogon::sync_wait(invitations.create(ask(UserRole::Guard), 61));
  CHECK(guard.invitation.role == UserRole::Guard);
  CHECK(scalar("SELECT COUNT(*) FROM user_invitation WHERE role = 'guard'") == 1);
  moduleGate().reset();
}

TEST_CASE("the app stops while every singleton it uses is still alive")
{
  runnerSlot().reset();
  CHECK(runnerSlot() == nullptr);
}
