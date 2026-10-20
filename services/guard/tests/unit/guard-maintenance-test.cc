#include "guard-fakes.hxx"

#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <feature/guard/guard-copy.hxx>
#include <feature/guard/guard-tamper-policy.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/repositories/maintenance/maintenance-repository.hxx>
#include <feature/guard/services/maintenance-service.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <test-support/value-of.hxx>

using guard_test::GuardBoot;
using guard_test::scalar;

namespace
{
GuardBoot& boot()
{
  static GuardBoot shared("guard-maintenance-test");
  return shared;
}

void exec(const std::string& sql)
{
  static_cast<void>(DbService::client()->execSqlSync(sql));
}

int64_t homeId()
{
  return std::stoll(scalar("SELECT id FROM guard_environment WHERE is_default = 1"));
}

int64_t officeId()
{
  const std::string found = scalar("SELECT id FROM guard_environment WHERE name = 'Oficina'");
  if (!found.empty())
    return std::stoll(found);
  exec("INSERT INTO guard_environment (name, kind) VALUES ('Oficina', 'office')");
  return std::stoll(scalar("SELECT id FROM guard_environment WHERE name = 'Oficina'"));
}

void assign(int64_t cameraId, int64_t environmentId)
{
  exec("INSERT INTO guard_camera_context (camera_id, environment_id) VALUES (" + std::to_string(cameraId) + ", " +
       std::to_string(environmentId) + ") ON CONFLICT (camera_id) DO UPDATE SET environment_id = excluded.environment_id");
}

std::vector<ResponseUser> household()
{
  return {{.userId = 1, .role = UserRole::Owner, .active = true, .name = "Ana", .lang = "es"},
          {.userId = 2, .role = UserRole::Resident, .active = true, .name = "Luis", .lang = "es"},
          {.userId = 4, .role = UserRole::Guard, .active = true, .name = "Pedro", .lang = "es"},
          {.userId = 5, .role = UserRole::Guest, .active = true, .name = "Invitado", .lang = "en"}};
}

class RelearningCameras final : public CameraActionClient
{
public:
  RelearningCameras() : CameraActionClient({.target = "127.0.0.1:1", .credential = {}}) {}

  bool relearn(int64_t cameraId) const override
  {
    relearned.store(cameraId);
    return true;
  }

  mutable std::atomic<int64_t> relearned{0};
};

struct Rig
{
  std::shared_ptr<NatsBus> bus = std::make_shared<NatsBus>();
  ResourceChangeOutbox outbox{bus};
  RelearningCameras cameras;
  std::map<int64_t, std::string> health;
  int64_t clock{1000000};
  MaintenanceService service{MaintenanceDependencies{
      .directory = std::make_shared<guard_test::RosterDirectory>(household()),
      .notifications = nullptr,
      .actions = &cameras,
      .changes = &outbox,
      .healthOf = [this](int64_t cameraId) {
        const auto found = health.find(cameraId);
        return found == health.end() ? std::string("ok") : found->second;
      },
      .cameraName = [](int64_t cameraId) { return "Cámara " + std::to_string(cameraId); },
      .config = [] { return MaintenanceConfig{.defaultMinutes = 15, .maxMinutes = 120, .duressWindowS = 900}; },
      .clock = [this] { return clock; }}};
};

std::string refusalOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.errorCode();
  }
  return {};
}
}

TEST_CASE("every tamper type waits its own window in protection and at home")
{
  const TamperWindows windows;
  const auto window = [&windows](std::string_view status, bool protect, bool sudden = false,
                                 std::string_view link = "wifi") {
    return guard_tamper::windowFor({.status = status,
                                    .sudden = sudden,
                                    .link = link,
                                    .protect = protect,
                                    .windows = windows,
                                    .homeS = 300,
                                    .offlineHomeS = 240});
  };
  CHECK(window("covered", true).afterS == 15);
  CHECK(window("covered", false).afterS == 300);
  CHECK(window("whiteout", true).afterS == 15);
  CHECK(window("blinded", true).afterS == 10);
  CHECK(window("blinded", false).afterS == 120);
  CHECK(window("partial_cover", true).afterS == 45);
  CHECK(window("moved", true).afterS == 20);
  CHECK(window("moved", false).afterS == 300);
  CHECK(window("dark_sudden", true).afterS == 45);
  CHECK(window("dark_sudden", false).action == TamperAction::Ignore);
  CHECK(window("unreachable", true, false, "wired").afterS == 15);
  CHECK(window("unreachable", true, false, "wifi").afterS == 30);
  CHECK(window("unreachable", false).afterS == 240);
  CHECK(window("blurred", true, true).action == TamperAction::Alarm);
  CHECK(window("blurred", true, true).afterS == 60);
  CHECK(window("blurred", true, false).action == TamperAction::LensNotice);
  CHECK(window("blurred", false, true).action == TamperAction::LensNotice);
  CHECK(window("dark", true).action == TamperAction::Ignore);
  CHECK(window("bright", true).action == TamperAction::Ignore);
}

TEST_CASE("cameras lost together form one group per environment, once")
{
  const std::vector<TamperReadingRef> readings{{.cameraId = 1, .environmentId = 10, .onsetAt = 100},
                                               {.cameraId = 2, .environmentId = 10, .onsetAt = 120},
                                               {.cameraId = 3, .environmentId = 10, .onsetAt = 400},
                                               {.cameraId = 4, .environmentId = 20, .onsetAt = 110}};
  const auto groups = guard_tamper::groupsOf(readings, 30);
  REQUIRE(groups.size() == 1);
  CHECK(groups.front().environmentId == 10);
  CHECK(groups.front().cameraIds == std::vector<int64_t>{1, 2});
  CHECK(groups.front().firstOnset == 100);
  CHECK(guard_tamper::groupDanger({.protect = true, .anyoneHome = true}) == GuardDanger::Critical);
  CHECK(guard_tamper::groupDanger({.protect = false, .anyoneHome = true}) == GuardDanger::High);
  CHECK(guard_tamper::groupDanger({.protect = false, .anyoneHome = false}) == GuardDanger::Critical);
}

TEST_CASE("a household face lowers the level, and stays critical when that person is away")
{
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Resident, .onDuty = false, .personAway = false,
                                   .protect = false}) == SeenVerdict::HomeNotice);
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Owner, .onDuty = false, .personAway = false,
                                   .protect = true}) == SeenVerdict::Lowered);
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Owner, .onDuty = false, .personAway = true,
                                   .protect = true}) == SeenVerdict::KeptCritical);
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Guard, .onDuty = true, .personAway = false,
                                   .protect = true}) == SeenVerdict::Lowered);
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Guard, .onDuty = false, .personAway = false,
                                   .protect = true}) == SeenVerdict::None);
  CHECK(guard_tamper::seenVerdict({.found = true, .role = UserRole::Guest, .onDuty = false, .personAway = false,
                                   .protect = false}) == SeenVerdict::GuestNamed);
  CHECK(guard_tamper::seenVerdict({.found = false, .role = UserRole::Owner, .onDuty = false, .personAway = false,
                                   .protect = false}) == SeenVerdict::None);
  CHECK(guard_tamper::approachingLens({.firstShare = 0.05, .lastShare = 0.2, .nearShare = 0.12}));
  CHECK_FALSE(guard_tamper::approachingLens({.firstShare = 0.3, .lastShare = 0.1, .nearShare = 0.12}));
}

TEST_CASE("Argus asks the people at home only at home, only with no face, and only when someone is there")
{
  CHECK(guard_tamper::shouldAsk({.protect = false, .seen = SeenVerdict::None, .anyoneHome = true}));
  CHECK_FALSE(guard_tamper::shouldAsk({.protect = true, .seen = SeenVerdict::None, .anyoneHome = true}));
  CHECK_FALSE(guard_tamper::shouldAsk({.protect = false, .seen = SeenVerdict::HomeNotice, .anyoneHome = true}));
  CHECK_FALSE(guard_tamper::shouldAsk({.protect = false, .seen = SeenVerdict::None, .anyoneHome = false}));
}

TEST_CASE("a declared window per role: the household anywhere, a guard only on duty and in its environment")
{
  static_cast<void>(boot());
  Rig rig;
  const int64_t home = homeId();
  const int64_t office = officeId();
  assign(11, home);
  assign(12, office);

  const Json::Value owner = drogon::sync_wait(rig.service.declare(
      {.actor = {.userId = 1, .role = UserRole::Owner}, .cameraIds = {11, 12}, .environmentId = 0, .minutes = 0,
       .source = MaintenanceSource::App}));
  CHECK(owner["active"].asBool());
  CHECK(owner["cameraIds"].size() == 2);
  CHECK(owner["endsAt"].asInt64() - owner["startedAt"].asInt64() == 15 * 60);

  const auto guardDeclares = [&rig](std::vector<int64_t> cameras) {
    return refusalOf([&rig, &cameras] {
      static_cast<void>(drogon::sync_wait(rig.service.declare({.actor = {.userId = 4, .role = UserRole::Guard},
                                                               .cameraIds = cameras,
                                                               .environmentId = 0,
                                                               .minutes = 30,
                                                               .source = MaintenanceSource::App})));
    });
  };
  CHECK(guardDeclares({11}) == "GUARD_OFF_DUTY");
  exec("INSERT INTO guard_response_recipient (environment_id, user_id, mode, step, on_duty, updated_at) VALUES (" +
       std::to_string(home) + ", 4, NULL, NULL, 1, 1)");
  CHECK(guardDeclares({11}).empty());
  CHECK(guardDeclares({12}) == "MAINTENANCE_OUT_OF_SCOPE");

  const auto refusedFor = [&rig](UserRole role) {
    return refusalOf([&rig, role] {
      static_cast<void>(drogon::sync_wait(rig.service.declare({.actor = {.userId = 5, .role = role},
                                                               .cameraIds = {11},
                                                               .environmentId = 0,
                                                               .minutes = 0,
                                                               .source = MaintenanceSource::App})));
    });
  };
  CHECK(refusedFor(UserRole::Guest) == "FORBIDDEN");
  CHECK(refusedFor(UserRole::Unknown) == "FORBIDDEN");
  CHECK(std::stoll(scalar("SELECT COUNT(*) FROM guard_maintenance")) >= 2);
}

TEST_CASE("a window declared under duress is flagged and covers nothing; an unknown person ends a window")
{
  static_cast<void>(boot());
  Rig rig;
  const int64_t home = homeId();
  assign(21, home);
  exec("INSERT INTO guard_safety_alert (kind, user_id, environment_id, created_at) VALUES ('duress', 2, " +
       std::to_string(home) + ", " + std::to_string(rig.clock - 60) + ")");
  const Json::Value forced = drogon::sync_wait(rig.service.declare(
      {.actor = {.userId = 2, .role = UserRole::Resident}, .cameraIds = {21}, .environmentId = 0, .minutes = 10,
       .source = MaintenanceSource::Assistant}));
  CHECK_FALSE(forced["duress"].asBool());
  const auto active = drogon::sync_wait(rig.service.activeWindows(rig.clock));
  const auto flagged = std::ranges::find(active, forced["id"].asInt64(), &MaintenanceWindow::id);
  REQUIRE(flagged != active.end());
  CHECK(flagged->duress);

  const Json::Value window = drogon::sync_wait(rig.service.declare(
      {.actor = {.userId = 1, .role = UserRole::Owner}, .cameraIds = {21}, .environmentId = 0, .minutes = 10,
       .source = MaintenanceSource::App}));
  drogon::sync_wait(rig.service.endForUnknownPerson({.environmentId = home, .at = rig.clock + 5}));
  MaintenanceRepository repository;
  const auto ended = valueOf(drogon::sync_wait(repository.findById(window["id"].asInt64())));
  CHECK(ended.endReason == MaintenanceEndReason::UnknownPerson);
}

TEST_CASE("the view at the end of a window is checked, and a confirmed view is relearned")
{
  static_cast<void>(boot());
  Rig rig;
  assign(31, homeId());
  assign(32, homeId());
  const Json::Value window = drogon::sync_wait(rig.service.declare(
      {.actor = {.userId = 2, .role = UserRole::Resident}, .cameraIds = {31, 32}, .environmentId = 0, .minutes = 5,
       .source = MaintenanceSource::App}));
  rig.health[31] = "moved";
  const Json::Value ended =
      drogon::sync_wait(rig.service.end({.actor = {.userId = 2, .role = UserRole::Resident}, .id = window["id"].asInt64()}));
  CHECK_FALSE(ended["active"].asBool());
  CHECK(ended["endReason"].asString() == "done");
  REQUIRE(ended["viewChecks"].size() == 1);
  CHECK(ended["viewChecks"][0]["cameraId"].asInt64() == 31);
  CHECK(ended["viewChecks"][0]["state"].asString() == "pending");

  CHECK(refusalOf([&rig, &window] {
          static_cast<void>(drogon::sync_wait(rig.service.answerView(
              {.actor = {.userId = 4, .role = UserRole::Guard}, .id = window["id"].asInt64(), .cameraId = 31,
               .accept = true})));
        }) == "FORBIDDEN");
  const Json::Value answered = drogon::sync_wait(rig.service.answerView(
      {.actor = {.userId = 2, .role = UserRole::Resident}, .id = window["id"].asInt64(), .cameraId = 31,
       .accept = true}));
  CHECK(answered["viewChecks"][0]["state"].asString() == "accepted");
  CHECK(rig.cameras.relearned.load() == 31);

  const Json::Value expired = drogon::sync_wait(rig.service.declare(
      {.actor = {.userId = 1, .role = UserRole::Owner}, .cameraIds = {32}, .environmentId = 0, .minutes = 1,
       .source = MaintenanceSource::App}));
  drogon::sync_wait(rig.service.expireDue(rig.clock + 61));
  MaintenanceRepository repository;
  const auto closed = valueOf(drogon::sync_wait(repository.findById(expired["id"].asInt64())));
  CHECK(closed.endReason == MaintenanceEndReason::Expired);
  CHECK(closed.viewChecks.empty());
}

TEST_CASE("the home question: yes declares a window, no is kept for the sweep, silence times out")
{
  static_cast<void>(boot());
  Rig rig;
  assign(41, homeId());
  MaintenanceRepository repository;
  const TamperAsk yes = drogon::sync_wait(repository.createAsk(
      {.cameraId = 41, .environmentId = homeId(), .onsetAt = 500, .askedAt = 510, .askedUsers = {1, 2}}));
  const Json::Value answered = drogon::sync_wait(
      rig.service.answerAsk({.actor = {.userId = 2, .role = UserRole::Resident}, .askId = yes.id,
                             .answer = TamperAskAnswer::Me}));
  CHECK(answered["answered"].asBool());
  const auto windows = drogon::sync_wait(rig.service.activeWindows(rig.clock));
  CHECK(std::ranges::any_of(windows, [](const MaintenanceWindow& window) {
    return window.covers(41) && window.source == MaintenanceSource::Reply;
  }));

  const TamperAsk no = drogon::sync_wait(repository.createAsk(
      {.cameraId = 41, .environmentId = homeId(), .onsetAt = 600, .askedAt = 610, .askedUsers = {1}}));
  static_cast<void>(drogon::sync_wait(rig.service.answerAsk(
      {.actor = {.userId = 1, .role = UserRole::Owner}, .askId = no.id, .answer = TamperAskAnswer::No})));
  CHECK(drogon::sync_wait(repository.findAsk(no.id))->answer == TamperAskAnswer::No);
  CHECK(refusalOf([&rig, &no] {
          static_cast<void>(drogon::sync_wait(rig.service.answerAsk(
              {.actor = {.userId = 1, .role = UserRole::Owner}, .askId = no.id, .answer = TamperAskAnswer::Me})));
        }) == "NOT_FOUND");

  const TamperAsk silent = drogon::sync_wait(repository.createAsk(
      {.cameraId = 41, .environmentId = homeId(), .onsetAt = 700, .askedAt = 710, .askedUsers = {1}}));
  CHECK(drogon::sync_wait(repository.answerAsk(
      {.id = silent.id, .answer = TamperAskAnswer::Timeout, .answeredBy = 0, .at = 770})));
  CHECK(drogon::sync_wait(repository.findAsk(silent.id))->answer == TamperAskAnswer::Timeout);
}

TEST_CASE("the lost-sight copy speaks Spanish and English")
{
  GuardNotice notice;
  notice.kind = NoticeKind::Tamper;
  notice.cameraName = "Patio";
  notice.tamperStatus = "blinded";
  notice.danger = GuardDanger::High;
  CHECK(guard_copy::render(notice, "es").title == "Perdí la visión de la cámara Patio");
  CHECK(guard_copy::render(notice, "es").body.find("deslumbrada") != std::string::npos);
  CHECK(guard_copy::render(notice, "en").title == "I lost sight of the Patio camera");
  notice.tamperStatus = "moved";
  CHECK(guard_copy::render(notice, "es").body.find("girada") != std::string::npos);
  notice.tamperStatus = "unreachable";
  CHECK(guard_copy::render(notice, "es").body.find("no recibe imagen") != std::string::npos);
  notice.lost = {{.cameraId = 1, .cameraName = "Patio", .count = 1}, {.cameraId = 2, .cameraName = "Puerta", .count = 1}};
  CHECK(guard_copy::render(notice, "es").title == "Perdí la visión de 2 cámaras a la vez");
  CHECK(guard_copy::render(notice, "en").title == "I lost sight of 2 cameras at once");
  notice.lost.clear();
  notice.danger = GuardDanger::Low;
  notice.probableMaintenance = true;
  notice.seenName = "Luis";
  CHECK(guard_copy::render(notice, "es").title == "Parece que Luis está manipulando la cámara Patio");
  notice.kind = NoticeKind::TamperAsk;
  CHECK(guard_copy::render(notice, "es").title == "¿Alguien está moviendo la cámara Patio?");
  CHECK(noticeKindToString(NoticeKind::TamperAsk) == "guard_tamper_ask");
  notice.kind = NoticeKind::ViewCheck;
  CHECK(guard_copy::render(notice, "es").title == "La cámara Patio quedó mirando distinto");
  CHECK(guard_copy::urgency(notice) == "active");
  notice.kind = NoticeKind::LensNotice;
  CHECK(guard_copy::render(notice, "es").title == "Limpia el lente de la cámara Patio");
  CHECK(guard_copy::urgency(notice) == "passive");
}
