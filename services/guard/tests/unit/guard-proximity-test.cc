#include <doctest/doctest.h>

#include <feature/proximity/infra/proximity-readings.hxx>
#include <feature/proximity/services/beacon-token.hxx>
#include <feature/proximity/services/proximity-service.hxx>
#include <feature/proximity/services/room-belief.hxx>

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <test-support/value-of.hxx>

namespace
{

constexpr std::string_view kSeed = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

class ProximityBoot
{
public:
  ProximityBoot() : db_("guard-proximity-test")
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{
        .connectionNumber = 1, .filename = db_.path(), .name = "default", .timeout = -1});
    runner_ = std::thread([] { drogon::app().run(); });
    if (!guard_test::waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH))
      throw std::runtime_error("guard schema apply failed");
  }

  ~ProximityBoot()
  {
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  ProximityBoot(const ProximityBoot&) = delete;
  ProximityBoot& operator=(const ProximityBoot&) = delete;

private:
  guard_test::TempDb db_;
  std::thread runner_;
};

ProximityBoot& boot()
{
  static ProximityBoot shared;
  return shared;
}

class FakeProbe final : public ProximityProbe
{
public:
  void hear(std::vector<BleHeard> heard)
  {
    std::scoped_lock lock(mutex_);
    heard_ = std::move(heard);
  }

  void radar(std::optional<bool> present)
  {
    std::scoped_lock lock(mutex_);
    present_ = present;
  }

  [[nodiscard]] drogon::Task<std::optional<std::vector<BleHeard>>>
  readBle(ProximityReceiverRow receiver) const override
  {
    static_cast<void>(receiver);
    std::scoped_lock lock(mutex_);
    co_return heard_;
  }

  [[nodiscard]] drogon::Task<std::optional<bool>> readRadar(ProximitySensorRow sensor) const override
  {
    static_cast<void>(sensor);
    std::scoped_lock lock(mutex_);
    co_return present_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<BleHeard> heard_;
  std::optional<bool> present_;
};

class FakeWorld final : public ProximityWorld
{
public:
  void located(std::unordered_set<int64_t> users)
  {
    std::scoped_lock lock(mutex_);
    located_ = std::move(users);
  }

  void see(std::vector<ProximitySighting> sightings)
  {
    std::scoped_lock lock(mutex_);
    sightings_ = std::move(sightings);
  }

  [[nodiscard]] drogon::Task<std::optional<std::vector<ProximitySighting>>> sightings(int64_t sinceMs) const override
  {
    std::scoped_lock lock(mutex_);
    std::vector<ProximitySighting> out;
    for (const ProximitySighting& seen : sightings_)
      if (seen.atMs >= sinceMs)
        out.push_back(seen);
    co_return out;
  }

  [[nodiscard]] drogon::Task<std::vector<ProximityCameraRoom>> cameraRooms() const override
  {
    co_return std::vector<ProximityCameraRoom>{{.cameraId = 1, .room = "1:living"},
                                               {.cameraId = 2, .room = "1:kitchen"}};
  }

  [[nodiscard]] drogon::Task<std::optional<std::unordered_set<int64_t>>> locatedUsers() const override
  {
    std::scoped_lock lock(mutex_);
    co_return located_;
  }

  [[nodiscard]] drogon::Task<std::optional<int64_t>> userOfPerson(int64_t personId) const override
  {
    co_return personId == 70 ? std::optional<int64_t>(7) : std::nullopt;
  }

private:
  mutable std::mutex mutex_;
  std::unordered_set<int64_t> located_{7, 8};
  std::vector<ProximitySighting> sightings_;
};

int64_t epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

RoomBeliefConfig beliefConfig()
{
  return {.weightFace = 1.0,
          .weightRadarBox = 0.8,
          .weightVoice = 0.6,
          .weightBle = 0.4,
          .nightFaceFactor = 0.5,
          .halfLifeMs = 20000,
          .switchMargin = 0.2,
          .switchHoldMs = 3000,
          .forgetMs = 300000};
}

}

TEST_CASE("the beacon token is the first sixteen bytes of HMAC-SHA256 over the big-endian counter")
{
  const auto seed = valueOf(beaconSeedBytes(kSeed));
  CHECK(beaconUuid(beaconToken(seed, 0)) == "9f0cd9b9-4097-fe49-2991-8d2b8942b344");
  CHECK(beaconUuid(beaconToken(seed, 1)) == "c432e059-c378-eef7-fe2f-1181a4050836");
  CHECK(beaconUuid(beaconToken(seed, 1902345)) == "adb473b3-b0e2-0243-9cca-c2b4f082d5ca");
}

TEST_CASE("the beacon counter is the floor of the time over the rotation")
{
  CHECK(beaconCounter({.nowSeconds = 1799, .rotateSeconds = 900}) == 1);
  CHECK(beaconCounter({.nowSeconds = 1800, .rotateSeconds = 900}) == 2);
  CHECK(beaconCounter({.nowSeconds = 5, .rotateSeconds = 0}) == 5);
}

TEST_CASE("a seed is 32 random bytes in base64 and a short or broken seed is refused")
{
  const std::string minted = mintBeaconSeed();
  const auto bytes = valueOf(beaconSeedBytes(minted));
  CHECK(bytes.size() == kBeaconSeedBytes);
  CHECK(mintBeaconSeed() != minted);
  CHECK_FALSE(beaconSeedBytes("AAEC").has_value());
  CHECK_FALSE(beaconSeedBytes("abc").has_value());
}

TEST_CASE("the beacon table answers the neighbouring rotations in both byte orders")
{
  const auto seed = valueOf(beaconSeedBytes(kSeed));
  const std::vector<BeaconSeed> seeds{{.userId = 7, .seed = seed}};
  const auto table = beaconTable({.seeds = seeds, .nowSeconds = 1902345LL * 900 + 10, .rotateSeconds = 900});
  const auto current = valueOf(normalizedUuid("ADB473B3-B0E2-0243-9CCA-C2B4F082D5CA"));
  CHECK(table.at(current) == 7);
  BeaconToken reversed = beaconToken(seed, 1902345);
  std::ranges::reverse(reversed);
  CHECK(table.at(*normalizedUuid(beaconUuid(reversed))) == 7);
  CHECK(table.contains(*normalizedUuid(beaconUuid(beaconToken(seed, 1902344)))));
  CHECK(table.contains(*normalizedUuid(beaconUuid(beaconToken(seed, 1902346)))));
  CHECK_FALSE(table.contains(*normalizedUuid(beaconUuid(beaconToken(seed, 1902343)))));
  CHECK_FALSE(normalizedUuid("not-a-uuid").has_value());
}

TEST_CASE("a receiver's text sensor lists the beacons it heard with their signal")
{
  Json::Value body(Json::objectValue);
  body["id"] = "text_sensor-argus_ble";
  body["value"] = "adb473b3-b0e2-0243-9cca-c2b4f082d5ca,-61; broken; 1234,abc;c432e059-c378-eef7-fe2f-1181a4050836,-80";
  const std::vector<BleHeard> heard = bleHeardOf(body);
  REQUIRE(heard.size() == 2);
  CHECK(heard.front().uuid == "adb473b3-b0e2-0243-9cca-c2b4f082d5ca");
  CHECK(heard.front().rssi == -61);
  CHECK(heard.back().rssi == -80);
  CHECK(bleHeardOf(Json::Value()).empty());
  CHECK(bleRequestPath("text_sensor.argus_ble").path == "/text_sensor/argus_ble");
}

TEST_CASE("radar presence reads the ESPHome and the Home Assistant shapes")
{
  Json::Value esphome(Json::objectValue);
  esphome["state"] = "ON";
  esphome["value"] = true;
  CHECK(radarPresenceOf(esphome) == std::optional<bool>(true));
  Json::Value assistant(Json::objectValue);
  assistant["state"] = "off";
  CHECK(radarPresenceOf(assistant) == std::optional<bool>(false));
  Json::Value unknown(Json::objectValue);
  unknown["state"] = "unavailable";
  CHECK_FALSE(radarPresenceOf(unknown).has_value());

  ProximitySensorRow fp2{.id = 1,
                         .name = "FP2",
                         .room = "1:living",
                         .model = ProximitySensorModel::Fp2,
                         .source = ProximitySensorSource::HomeAssistant,
                         .host = "ha.local",
                         .port = 8123,
                         .entityId = "binary_sensor.fp2_zone_1",
                         .cameraId = std::nullopt,
                         .panOffsetDeg = std::nullopt,
                         .createdAt = 0};
  CHECK(radarRequestPath(fp2).path == "/api/states/binary_sensor.fp2_zone_1");
  CHECK(radarRequestPath(fp2).bearer);
  ProximitySensorRow ld2410 = fp2;
  ld2410.model = ProximitySensorModel::Ld2410;
  ld2410.source = ProximitySensorSource::Esphome;
  ld2410.entityId = "binary_sensor.presence";
  CHECK(radarRequestPath(ld2410).path == "/binary_sensor/presence");
  CHECK_FALSE(radarRequestPath(ld2410).bearer);
}

TEST_CASE("a face outweighs a phone, and at night it counts half")
{
  const RoomBeliefConfig config = beliefConfig();
  CHECK(evidenceWeight({.userId = 1, .room = "a", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                        .night = false, .atMs = 0},
                       config) > evidenceWeight({.userId = 1, .room = "a", .kind = ProximityEvidenceKind::Ble,
                                                 .strength = 1.0, .night = false, .atMs = 0},
                                                config));
  CHECK(evidenceWeight({.userId = 1, .room = "a", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                        .night = true, .atMs = 0},
                       config) == doctest::Approx(0.5));
  CHECK(decayFactor(20000, 20000) == doctest::Approx(0.5));
  CHECK(bleStrength({.rssi = -95, .rssiFloor = 95}) == doctest::Approx(0.0));
  CHECK(bleStrength({.rssi = -40, .rssiFloor = 95}) == doctest::Approx(1.0));
  CHECK(voiceStrength({.userId = 1, .cameraId = 1, .snrDb = 23.0, .voiceprint = false, .atMs = 0}) ==
        doctest::Approx(0.5));
}

TEST_CASE("the room switches only after the new room has led by the margin for the hold")
{
  RoomBelief belief(beliefConfig());
  belief.observe({.userId = 7, .room = "living", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                  .night = false, .atMs = 1000});
  auto verdict = valueOf(belief.verdict({.userId = 7, .nowMs = 1000}));
  CHECK(verdict.room == "living");
  CHECK(verdict.strongest == ProximityEvidenceKind::Face);

  belief.observe({.userId = 7, .room = "kitchen", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                  .night = false, .atMs = 2000});
  belief.observe({.userId = 7, .room = "kitchen", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                  .night = false, .atMs = 2500});
  CHECK(belief.verdict({.userId = 7, .nowMs = 3000})->room == "living");
  belief.observe({.userId = 7, .room = "kitchen", .kind = ProximityEvidenceKind::Face, .strength = 1.0,
                  .night = false, .atMs = 5000});
  CHECK(belief.verdict({.userId = 7, .nowMs = 5600})->room == "kitchen");

  belief.observe({.userId = 7, .room = "living", .kind = ProximityEvidenceKind::Ble, .strength = 0.2,
                  .night = false, .atMs = 6000});
  CHECK(belief.verdict({.userId = 7, .nowMs = 12000})->room == "kitchen");
  CHECK_FALSE(belief.verdict({.userId = 7, .nowMs = 400000}).has_value());
  CHECK_FALSE(belief.verdict({.userId = 8, .nowMs = 1000}).has_value());
}

TEST_CASE("a member's beacon is minted on opt-in and a member who opted out of location is never located")
{
  boot();
  FakeProbe probe;
  FakeWorld world;
  GuardProximityConfig config;
  config.switchHoldMs = 0;
  ProximityService service({.probe = &probe, .world = &world, .config = config});

  const ProximityMeView before = drogon::sync_wait(service.me(7));
  CHECK_FALSE(before.enabled);
  CHECK_FALSE(before.seed.has_value());
  CHECK(before.located);
  CHECK(before.rotateSeconds == 900);

  const ProximityMeView after = drogon::sync_wait(service.setBle({.userId = 7, .enabled = true}));
  REQUIRE(after.seed.has_value());
  CHECK(after.enabled);
  const ProximityMeView again = drogon::sync_wait(service.setBle({.userId = 7, .enabled = true}));
  CHECK(again.seed == after.seed);

  world.located({7});
  CHECK_THROWS_AS(drogon::sync_wait(service.setBle({.userId = 8, .enabled = true})), ResponseException);
  CHECK_FALSE(drogon::sync_wait(service.me(8)).located);

  const ProximityReceiverView receiver = drogon::sync_wait(service.addReceiver(
      {.name = "Sala", .room = "1:living", .kind = ProximityReceiverKind::Esphome, .host = "10.0.0.5", .port = 80}));
  CHECK(receiver.row.id > 0);
  CHECK(drogon::sync_wait(service.me(7)).receivers == 1);

  const auto seed = valueOf(beaconSeedBytes(*after.seed));
  const int64_t counter = beaconCounter({.nowSeconds = epochMs() / 1000, .rotateSeconds = config.rotateSeconds});
  probe.hear({{.uuid = beaconUuid(beaconToken(seed, static_cast<uint64_t>(counter))), .rssi = -50}});
  drogon::sync_wait(service.poll());
  const std::vector<RoomVerdict> people = drogon::sync_wait(service.people());
  REQUIRE(people.size() == 1);
  CHECK(people.front().userId == 7);
  CHECK(people.front().room == "1:living");
  CHECK(people.front().strongest == ProximityEvidenceKind::Ble);
  CHECK(drogon::sync_wait(service.receivers()).front().online);

  world.see({{.cameraId = 2, .personId = 70, .known = true, .night = false, .atMs = epochMs()}});
  drogon::sync_wait(service.poll());
  drogon::sync_wait(service.poll());
  CHECK(drogon::sync_wait(service.people()).front().room == "1:kitchen");

  world.located({});
  drogon::sync_wait(service.poll());
  CHECK(drogon::sync_wait(service.people()).empty());

  drogon::sync_wait(service.removeReceiver(receiver.row.id));
  CHECK_THROWS_AS(drogon::sync_wait(service.removeReceiver(receiver.row.id)), ResponseException);
}

TEST_CASE("a sensor is registered with its room and its optional camera mapping")
{
  boot();
  FakeProbe probe;
  FakeWorld world;
  ProximityService service({.probe = &probe, .world = &world, .config = {}});
  const ProximitySensorView added = drogon::sync_wait(service.addSensor({.name = "Radar sala",
                                                                         .room = "1:living",
                                                                         .model = ProximitySensorModel::Ld2450,
                                                                         .source = ProximitySensorSource::Esphome,
                                                                         .host = "10.0.0.9",
                                                                         .port = 80,
                                                                         .entityId = "presence",
                                                                         .cameraId = 1,
                                                                         .panOffsetDeg = -30.0}));
  const std::vector<ProximitySensorView> sensors = drogon::sync_wait(service.sensors());
  REQUIRE_FALSE(sensors.empty());
  CHECK(sensors.back().row.id == added.row.id);
  CHECK(sensors.back().row.cameraId == std::optional<int64_t>(1));
  CHECK(sensors.back().row.panOffsetDeg == std::optional<double>(-30.0));
  probe.radar(true);
  drogon::sync_wait(service.poll());
  CHECK(drogon::sync_wait(service.sensors()).back().online);
  drogon::sync_wait(service.removeSensor(added.row.id));
}
