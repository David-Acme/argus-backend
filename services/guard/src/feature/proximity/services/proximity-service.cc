#include "proximity-service.hxx"

#include <feature/proximity/proximity-errors.hxx>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
constexpr int64_t kPersonCacheMs = 600000;
constexpr double kVoiceSnrFloorDb = 3.0;
constexpr double kVoiceSnrSpanDb = 20.0;
constexpr double kVoiceUnconfirmed = 0.5;
constexpr int64_t kBleStrongDbm = 40;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

RoomBeliefConfig roomBeliefConfigOf(const GuardProximityConfig& config)
{
  return {.weightFace = config.weightFace,
          .weightRadarBox = config.weightRadarBox,
          .weightVoice = config.weightVoice,
          .weightBle = config.weightBle,
          .nightFaceFactor = config.nightFaceFactor,
          .halfLifeMs = config.halfLifeMs,
          .switchMargin = config.switchMargin,
          .switchHoldMs = config.switchHoldMs,
          .forgetMs = config.forgetMs};
}

double bleStrength(const BleReading& reading)
{
  const auto floor = static_cast<double>(-reading.rssiFloor);
  const auto span = static_cast<double>(reading.rssiFloor - kBleStrongDbm);
  if (span <= 0.0)
    return 0.0;
  return std::clamp((static_cast<double>(reading.rssi) - floor) / span, 0.0, 1.0);
}

double voiceStrength(const ProximityVoiceEvidence& evidence)
{
  const double snr = std::clamp((evidence.snrDb - kVoiceSnrFloorDb) / kVoiceSnrSpanDb, 0.0, 1.0);
  return snr * (evidence.voiceprint ? 1.0 : kVoiceUnconfirmed);
}

ProximityService::ProximityService(ProximityServiceInput input)
    : input_(std::move(input)), belief_(roomBeliefConfigOf(input_.config)), config_(input_.config)
{
}

GuardProximityConfig ProximityService::config() const
{
  std::scoped_lock lock(mutex_);
  return config_;
}

void ProximityService::refresh(const GuardProximityConfig& config)
{
  {
    std::scoped_lock lock(mutex_);
    config_ = config;
  }
  belief_.configure(roomBeliefConfigOf(config));
}

void ProximityService::start()
{
  if (!config().enabled) {
    LOG_INFO << "Proximity: disabled by [guard.proximity] enabled";
    return;
  }
  if (started_.exchange(true))
    return;
  const auto schedule = std::make_shared<std::function<void()>>();
  *schedule = [this, weak = std::weak_ptr<std::function<void()>>(schedule)]() {
    if (stopping_.load())
      return;
    const auto again = weak.lock();
    drogon::app().getLoop()->runAfter(static_cast<double>(config().pollMs) / 1000.0, [this, again]() {
      if (stopping_.load())
        return;
      drogon::async_run([this, again]() -> drogon::Task<void> {
        co_await poll();
        if (again)
          (*again)();
      });
    });
  };
  scheduler_ = schedule;
  (*schedule)();
}

void ProximityService::requestStop()
{
  stopping_.store(true);
  scheduler_.reset();
}

bool ProximityService::drained() const
{
  return !polling_.load();
}

drogon::Task<std::optional<bool>> ProximityService::located(int64_t userId) const
{
  if (input_.world == nullptr)
    co_return true;
  const auto users = co_await input_.world->locatedUsers();
  if (!users)
    co_return std::nullopt;
  co_return users->contains(userId);
}

drogon::Task<ProximityMeView> ProximityService::me(int64_t userId) const
{
  const auto row = co_await beaconRepository_.find(userId);
  const int64_t receivers = co_await receiverRepository_.count();
  const bool locatedNow = (co_await located(userId)).value_or(true);
  const bool enabled = row && row->enabled && locatedNow;
  co_return ProximityMeView{.enabled = enabled,
                            .seed = enabled ? std::optional<std::string>(row->seed) : std::nullopt,
                            .rotateSeconds = config().rotateSeconds,
                            .receivers = receivers,
                            .located = locatedNow};
}

drogon::Task<ProximityMeView> ProximityService::setBle(ProximityBleChange change) const
{
  const int64_t now = nowMs() / 1000;
  if (!change.enabled) {
    co_await beaconRepository_.disable(change.userId, now);
    co_return co_await me(change.userId);
  }
  const std::optional<bool> allowed = co_await located(change.userId);
  if (!allowed)
    throw ResponseException(ProximityErrors::LocationUnknown);
  if (!*allowed)
    throw ResponseException(ProximityErrors::LocationOff);
  const auto existing = co_await beaconRepository_.find(change.userId);
  co_await beaconRepository_.upsert({.userId = change.userId,
                                     .seed = existing ? existing->seed : mintBeaconSeed(),
                                     .enabled = true,
                                     .now = now});
  co_return co_await me(change.userId);
}

std::optional<int64_t> ProximityService::seenAt(const std::unordered_map<int64_t, int64_t>& seen, int64_t id) const
{
  std::scoped_lock lock(mutex_);
  const auto found = seen.find(id);
  if (found == seen.end())
    return std::nullopt;
  return found->second;
}

void ProximityService::markSeen(std::unordered_map<int64_t, int64_t>& seen, int64_t id, int64_t atMs)
{
  std::scoped_lock lock(mutex_);
  seen[id] = atMs;
}

bool ProximityService::onlineAt(std::optional<int64_t> seenMs, int64_t now) const
{
  return seenMs && now - *seenMs <= config().offlineAfterMs;
}

drogon::Task<std::vector<ProximityReceiverView>> ProximityService::receivers() const
{
  const int64_t now = nowMs();
  std::vector<ProximityReceiverView> views;
  for (ProximityReceiverRow& row : co_await receiverRepository_.find()) {
    const std::optional<int64_t> seen = seenAt(receiverSeenMs_, row.id);
    views.push_back({.row = std::move(row),
                     .online = onlineAt(seen, now),
                     .lastSeenAt = seen ? std::optional<int64_t>(*seen / 1000) : std::nullopt});
  }
  co_return views;
}

drogon::Task<ProximityReceiverView> ProximityService::addReceiver(ProximityReceiverAdd add) const
{
  ProximityReceiverRow row = co_await receiverRepository_.create({.name = std::move(add.name),
                                                                  .room = std::move(add.room),
                                                                  .kind = add.kind,
                                                                  .host = std::move(add.host),
                                                                  .port = add.port,
                                                                  .now = nowMs() / 1000});
  co_return ProximityReceiverView{.row = std::move(row), .online = false, .lastSeenAt = std::nullopt};
}

drogon::Task<void> ProximityService::removeReceiver(int64_t id) const
{
  if (!co_await receiverRepository_.remove(id))
    throw ResponseException(ProximityErrors::ReceiverNotFound);
}

drogon::Task<std::vector<ProximitySensorView>> ProximityService::sensors() const
{
  const int64_t now = nowMs();
  std::vector<ProximitySensorView> views;
  for (ProximitySensorRow& row : co_await sensorRepository_.find()) {
    const std::optional<int64_t> seen = seenAt(sensorSeenMs_, row.id);
    views.push_back({.row = std::move(row),
                     .online = onlineAt(seen, now),
                     .lastSeenAt = seen ? std::optional<int64_t>(*seen / 1000) : std::nullopt});
  }
  co_return views;
}

drogon::Task<ProximitySensorView> ProximityService::addSensor(ProximitySensorAdd add) const
{
  ProximitySensorRow row = co_await sensorRepository_.create({.name = std::move(add.name),
                                                              .room = std::move(add.room),
                                                              .model = add.model,
                                                              .source = add.source,
                                                              .host = std::move(add.host),
                                                              .port = add.port,
                                                              .entityId = std::move(add.entityId),
                                                              .cameraId = add.cameraId,
                                                              .panOffsetDeg = add.panOffsetDeg,
                                                              .now = nowMs() / 1000});
  co_return ProximitySensorView{.row = std::move(row), .online = false, .lastSeenAt = std::nullopt};
}

drogon::Task<void> ProximityService::removeSensor(int64_t id) const
{
  if (!co_await sensorRepository_.remove(id))
    throw ResponseException(ProximityErrors::SensorNotFound);
}

drogon::Task<std::vector<RoomVerdict>> ProximityService::people() const
{
  std::optional<std::unordered_set<int64_t>> allowed;
  if (input_.world != nullptr)
    allowed = co_await input_.world->locatedUsers();
  std::vector<RoomVerdict> verdicts = belief_.all(nowMs());
  if (!allowed) {
    verdicts.clear();
    co_return verdicts;
  }
  std::erase_if(verdicts, [&allowed](const RoomVerdict& verdict) { return !allowed->contains(verdict.userId); });
  co_return verdicts;
}

void ProximityService::observeLocated(const RoomEvidence& evidence)
{
  {
    std::scoped_lock lock(mutex_);
    if (!locatedKnown_ || !located_.contains(evidence.userId))
      return;
  }
  belief_.observe(evidence);
}

drogon::Task<bool> ProximityService::reportVoice(ProximityVoiceEvidence evidence)
{
  if (!config().enabled || evidence.userId <= 0 || evidence.cameraId <= 0)
    co_return false;
  std::string room;
  {
    std::scoped_lock lock(mutex_);
    if (const auto found = rooms_.find(evidence.cameraId); found != rooms_.end())
      room = found->second;
  }
  if (room.empty()) {
    const auto rooms = co_await refreshRooms();
    if (const auto found = rooms.find(evidence.cameraId); found != rooms.end())
      room = found->second;
  }
  if (room.empty())
    co_return false;
  observeLocated({.userId = evidence.userId,
                  .room = room,
                  .kind = ProximityEvidenceKind::Voice,
                  .strength = voiceStrength(evidence),
                  .night = false,
                  .atMs = evidence.atMs > 0 ? evidence.atMs : nowMs()});
  co_return true;
}

drogon::Task<std::unordered_set<int64_t>> ProximityService::refreshLocated()
{
  std::optional<std::unordered_set<int64_t>> fresh;
  if (input_.world != nullptr)
    fresh = co_await input_.world->locatedUsers();
  std::vector<int64_t> dropped;
  std::unordered_set<int64_t> current;
  {
    std::scoped_lock lock(mutex_);
    if (fresh) {
      for (const int64_t userId : located_)
        if (!fresh->contains(userId))
          dropped.push_back(userId);
      located_ = std::move(*fresh);
      locatedKnown_ = true;
    }
    current = located_;
  }
  for (const int64_t userId : dropped)
    belief_.forget(userId);
  co_return current;
}

drogon::Task<std::unordered_map<int64_t, std::string>> ProximityService::refreshRooms()
{
  if (input_.world == nullptr)
    co_return std::unordered_map<int64_t, std::string>{};
  std::unordered_map<int64_t, std::string> fresh;
  for (const ProximityCameraRoom& row : co_await input_.world->cameraRooms())
    if (!row.room.empty())
      fresh.emplace(row.cameraId, row.room);
  std::scoped_lock lock(mutex_);
  rooms_ = fresh;
  co_return fresh;
}

drogon::Task<std::optional<int64_t>> ProximityService::userOf(int64_t personId)
{
  if (personId <= 0 || input_.world == nullptr)
    co_return std::nullopt;
  {
    std::scoped_lock lock(mutex_);
    if (const auto found = personUsers_.find(personId); found != personUsers_.end())
      co_return found->second;
  }
  const std::optional<int64_t> userId = co_await input_.world->userOfPerson(personId);
  std::scoped_lock lock(mutex_);
  personUsers_[personId] = userId;
  co_return userId;
}

drogon::Task<void> ProximityService::poll()
{
  if (polling_.exchange(true))
    co_return;
  try {
    const GuardProximityConfig settings = config();
    const int64_t now = nowMs();
    {
      std::scoped_lock lock(mutex_);
      if (now - personCacheAtMs_ > kPersonCacheMs) {
        personUsers_.clear();
        personCacheAtMs_ = now;
      }
    }
    const std::unordered_set<int64_t> located = co_await refreshLocated();
    const std::unordered_map<int64_t, std::string> rooms = co_await refreshRooms();

    if (input_.world != nullptr) {
      int64_t from = 0;
      {
        std::scoped_lock lock(mutex_);
        from = std::max(sightingsFromMs_, now - settings.sightingWindowMs);
      }
      if (const auto sightings = co_await input_.world->sightings(from)) {
        int64_t newest = from - 1;
        for (const ProximitySighting& seen : *sightings) {
          if (seen.atMs < from)
            continue;
          newest = std::max(newest, seen.atMs);
          const auto room = rooms.find(seen.cameraId);
          if (room == rooms.end())
            continue;
          {
            std::scoped_lock lock(mutex_);
            int64_t& box = boxSeenMs_[room->second];
            box = std::max(box, seen.atMs);
          }
          if (!seen.known)
            continue;
          const std::optional<int64_t> userId = co_await userOf(seen.personId);
          if (userId && located.contains(*userId))
            observeLocated({.userId = *userId,
                            .room = room->second,
                            .kind = ProximityEvidenceKind::Face,
                            .strength = 1.0,
                            .night = seen.night,
                            .atMs = seen.atMs});
        }
        std::scoped_lock lock(mutex_);
        sightingsFromMs_ = std::max(from, newest + 1);
      }
    }

    if (input_.probe != nullptr) {
      std::vector<std::string> radarRooms;
      for (ProximitySensorRow& sensor : co_await sensorRepository_.find()) {
        const int64_t sensorId = sensor.id;
        const std::string room = sensor.room;
        const std::optional<bool> present = co_await input_.probe->readRadar(std::move(sensor));
        if (!present)
          continue;
        markSeen(sensorSeenMs_, sensorId, nowMs());
        if (*present && !room.empty())
          radarRooms.push_back(room);
      }
      for (const std::string& room : radarRooms) {
        bool boxed = false;
        {
          std::scoped_lock lock(mutex_);
          const auto box = boxSeenMs_.find(room);
          boxed = box != boxSeenMs_.end() && now - box->second <= settings.sightingWindowMs;
        }
        if (!boxed)
          continue;
        for (const RoomVerdict& verdict : belief_.all(now))
          if (verdict.room == room)
            observeLocated({.userId = verdict.userId,
                            .room = room,
                            .kind = ProximityEvidenceKind::RadarBox,
                            .strength = 1.0,
                            .night = false,
                            .atMs = now});
      }

      const std::vector<ProximityBeaconRow> beacons = co_await beaconRepository_.findEnabled();
      std::vector<BeaconSeed> seeds;
      seeds.reserve(beacons.size());
      for (const ProximityBeaconRow& beacon : beacons) {
        if (!located.contains(beacon.userId))
          continue;
        if (auto bytes = beaconSeedBytes(beacon.seed))
          seeds.push_back({.userId = beacon.userId, .seed = std::move(*bytes)});
      }
      const auto table = beaconTable({.seeds = seeds, .nowSeconds = now / 1000, .rotateSeconds = settings.rotateSeconds});
      for (ProximityReceiverRow& receiver : co_await receiverRepository_.find()) {
        const int64_t receiverId = receiver.id;
        const std::string room = receiver.room;
        const auto heard = co_await input_.probe->readBle(std::move(receiver));
        if (!heard)
          continue;
        markSeen(receiverSeenMs_, receiverId, nowMs());
        if (room.empty() || table.empty())
          continue;
        for (const BleHeard& beacon : *heard) {
          const auto uuid = normalizedUuid(beacon.uuid);
          if (!uuid)
            continue;
          const auto owner = table.find(*uuid);
          if (owner == table.end())
            continue;
          const double strength = bleStrength({.rssi = beacon.rssi, .rssiFloor = settings.bleRssiFloor});
          if (strength <= 0.0)
            continue;
          observeLocated({.userId = owner->second,
                          .room = room,
                          .kind = ProximityEvidenceKind::Ble,
                          .strength = strength,
                          .night = false,
                          .atMs = now});
        }
      }
    }
  }
  catch (const std::exception& error) {
    LOG_WARN << "Proximity: the poll failed: " << error.what();
  }
  polling_.store(false);
}
