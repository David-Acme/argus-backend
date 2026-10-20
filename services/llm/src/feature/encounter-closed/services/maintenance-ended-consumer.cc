#include "maintenance-ended-consumer.hxx"

#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <drogon/drogon.h>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kSecondsPerHour = 3600;

constexpr std::array<std::string_view, 7> kWeekdaysEs{"domingo", "lunes", "martes", "miércoles",
                                                      "jueves", "viernes", "sábado"};
constexpr std::array<std::string_view, 7> kWeekdaysEn{"Sunday", "Monday", "Tuesday", "Wednesday",
                                                      "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 12> kMonthsEs{"enero", "febrero", "marzo", "abril", "mayo", "junio",
                                                     "julio", "agosto", "septiembre", "octubre", "noviembre",
                                                     "diciembre"};
constexpr std::array<std::string_view, 12> kMonthsEn{"January", "February", "March", "April", "May", "June",
                                                     "July", "August", "September", "October", "November",
                                                     "December"};

std::string maintenanceFingerprint(const std::string& payload)
{
  return argus::hash::sha256Hex(json_util::toString(json_util::fromString(payload)));
}

struct LocalMoment
{
  unsigned weekday{0};
  unsigned month{1};
  unsigned day{1};
  int64_t hour{0};
  int64_t minute{0};
};

struct MomentInput
{
  int64_t at{0};
  int offsetSeconds{0};
};

LocalMoment momentOf(const MomentInput& input)
{
  const std::chrono::sys_seconds local{std::chrono::seconds{input.at + input.offsetSeconds}};
  const auto day = std::chrono::floor<std::chrono::days>(local);
  const std::chrono::year_month_day date{day};
  const int64_t seconds = (local - day).count();
  return {.weekday = std::chrono::weekday{day}.c_encoding(),
          .month = static_cast<unsigned>(date.month()),
          .day = static_cast<unsigned>(date.day()),
          .hour = seconds / kSecondsPerHour,
          .minute = (seconds % kSecondsPerHour) / kSecondsPerMinute};
}

std::string clockOf(const LocalMoment& moment)
{
  std::string minutes = std::to_string(moment.minute);
  if (minutes.size() < 2)
    minutes.insert(0, "0");
  return std::to_string(moment.hour) + ":" + minutes;
}

std::string listOf(const std::vector<std::string>& names, bool english)
{
  std::string out;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0)
      out += index + 1 == names.size() ? (english ? " and " : " y ") : ", ";
    out += names.at(index);
  }
  return out;
}

std::string camerasPhrase(const MaintenanceEndedEvent& event, bool english)
{
  if (event.cameras.empty())
    return english ? "on the cameras" : "en las cámaras";
  if (event.cameras.size() == 1)
    return english ? "on the " + event.cameras.front() + " camera" : "en la cámara " + event.cameras.front();
  return english ? "on the " + listOf(event.cameras, true) + " cameras" : "en las cámaras " + listOf(event.cameras, false);
}

std::string_view reasonPhrase(const MaintenanceEndedEvent& event, bool english)
{
  if (event.endReason == "unknown_person")
    return english ? " It ended when an unknown person appeared." : " Terminó porque apareció una persona desconocida.";
  if (event.endReason == "expired")
    return english ? " It ended on its own when the time ran out." : " Terminó solo al acabarse el tiempo.";
  return {};
}
}

struct MaintenanceLifecycle
{
  std::atomic<bool> alive{true};
  std::atomic<int> active{0};
  std::mutex mutex;
  std::condition_variable idle;
};

namespace
{
class MaintenanceLifecycleGuard
{
public:
  explicit MaintenanceLifecycleGuard(std::shared_ptr<MaintenanceLifecycle> lifecycle)
      : lifecycle_(std::move(lifecycle))
  {
    if (lifecycle_)
      lifecycle_->active.fetch_add(1, std::memory_order_acq_rel);
  }

  ~MaintenanceLifecycleGuard()
  {
    if (!lifecycle_)
      return;
    if (lifecycle_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      const std::scoped_lock lock(lifecycle_->mutex);
      lifecycle_->idle.notify_all();
    }
  }

  MaintenanceLifecycleGuard(const MaintenanceLifecycleGuard&) = delete;
  MaintenanceLifecycleGuard& operator=(const MaintenanceLifecycleGuard&) = delete;

  [[nodiscard]] bool alive() const
  {
    return lifecycle_ != nullptr && lifecycle_->alive.load(std::memory_order_acquire);
  }

private:
  std::shared_ptr<MaintenanceLifecycle> lifecycle_;
};
}

std::optional<MaintenanceEndedEvent> MaintenanceEndedEvent::fromJson(const Json::Value& json)
{
  if (!json.isObject())
    return std::nullopt;
  MaintenanceEndedEvent event;
  event.eventId = json.get("eventId", "").asString();
  event.maintenanceId = json.get("maintenanceId", 0).asInt64();
  event.environment = json.get("environment", "").asString();
  if (const Json::Value& cameras = json["cameras"]; cameras.isArray())
    for (const Json::Value& camera : cameras)
      if (camera.isString() && !camera.asString().empty())
        event.cameras.push_back(camera.asString());
  event.declaredBy = json.get("declaredBy", 0).asInt64();
  event.declaredName = json.get("declaredName", "").asString();
  event.endReason = json.get("endReason", "").asString();
  event.startedAt = json.get("startedAt", 0).asInt64();
  event.endedAt = json.get("endedAt", 0).asInt64();
  if (event.eventId.empty() || event.maintenanceId <= 0 || event.startedAt <= 0 || event.endedAt < event.startedAt)
    return std::nullopt;
  return event;
}

namespace maintenance_episode
{

std::string summary(const MaintenanceSummaryInput& input)
{
  const MaintenanceEndedEvent& event = input.event;
  const bool english = input.lang == "en";
  const LocalMoment from = momentOf({.at = event.startedAt, .offsetSeconds = input.utcOffsetSeconds});
  const LocalMoment to = momentOf({.at = event.endedAt, .offsetSeconds = input.utcOffsetSeconds});
  const int64_t minutes = std::max<int64_t>(1, (event.endedAt - event.startedAt) / kSecondsPerMinute);
  const std::string who = event.declaredName.empty() ? (english ? "someone at home" : "alguien de casa")
                                                     : event.declaredName;
  const std::string place =
      event.environment.empty() ? std::string() : (english ? " at " : " de ") + event.environment;
  if (english)
    return "Camera maintenance (technician visit or cleaning) declared by " + who + ", " +
           camerasPhrase(event, true) + place + ", on " + std::string(kWeekdaysEn.at(from.weekday)) + ", " +
           std::string(kMonthsEn.at(from.month - 1)) + " " + std::to_string(from.day) + " from " + clockOf(from) +
           " to " + clockOf(to) + " (" + std::to_string(minutes) + " min)." + std::string(reasonPhrase(event, true));
  return "Mantenimiento de cámaras (visita del técnico o limpieza) declarado por " + who + ", " +
         camerasPhrase(event, false) + place + ", el " + std::string(kWeekdaysEs.at(from.weekday)) + " " +
         std::to_string(from.day) + " de " + std::string(kMonthsEs.at(from.month - 1)) + " de " + clockOf(from) +
         " a " + clockOf(to) + " (" + std::to_string(minutes) + " min)." + std::string(reasonPhrase(event, false));
}

}

MaintenanceEndedConsumer::MaintenanceEndedConsumer(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)), config_(std::move(config)),
      lifecycle_(std::make_shared<MaintenanceLifecycle>())
{
}

MaintenanceEndedConsumer::~MaintenanceEndedConsumer()
{
  stop();
}

void MaintenanceEndedConsumer::start()
{
  if (dependencies_.bus == nullptr)
    return;
  if (trySubscribe())
    return;
  LOG_WARN << "Maintenance consumer: stream not ready; retrying";
  scheduleSubscribeRetry();
}

void MaintenanceEndedConsumer::stop()
{
  lifecycle_->alive.store(false, std::memory_order_release);
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  if (subscription_.has_value() && dependencies_.bus != nullptr)
    dependencies_.bus->unsubscribe(*subscription_);
  subscription_.reset();
  std::unique_lock lock(lifecycle_->mutex);
  lifecycle_->idle.wait(lock, [lifecycle = lifecycle_] {
    return lifecycle->active.load(std::memory_order_acquire) == 0;
  });
}

void MaintenanceEndedConsumer::requestStop()
{
  lifecycle_->alive.store(false, std::memory_order_release);
}

bool MaintenanceEndedConsumer::drained() const
{
  return lifecycle_->active.load(std::memory_order_acquire) == 0;
}

drogon::Task<MaintenanceDisposition> MaintenanceEndedConsumer::handlePayload(const std::string& payload)
{
  co_return settlePayload(payload);
}

MaintenanceDisposition MaintenanceEndedConsumer::settlePayload(const std::string& payload)
{
  const MaintenanceLifecycleGuard guard(lifecycle_);
  if (!guard.alive())
    return MaintenanceDisposition::Nak;
  const auto event = MaintenanceEndedEvent::fromJson(json_util::fromString(payload));
  if (!event) {
    LOG_WARN << "Maintenance consumer: dropped malformed payload";
    return MaintenanceDisposition::Term;
  }
  return settleEvent(*event, maintenanceFingerprint(payload));
}

MaintenanceDisposition MaintenanceEndedConsumer::settleEvent(const MaintenanceEndedEvent& event,
                                                             const std::string& fingerprint)
{
  if (dependencies_.graph == nullptr || dependencies_.repository == nullptr || !dependencies_.capture) {
    LOG_ERROR << "Maintenance consumer: dependencies missing; redelivering";
    return MaintenanceDisposition::Nak;
  }
  const auto now = static_cast<int64_t>(std::time(nullptr));
  EncounterClosedClaim claim{.duplicate = true};
  {
    const std::scoped_lock lock(dependencies_.graph->mutex());
    claim = dependencies_.repository->claimEncounterClosed(
        dependencies_.graph->handle(), {.eventId = event.eventId, .fingerprint = fingerprint, .at = now});
  }
  if (claim.duplicate)
    return MaintenanceDisposition::Ack;
  int64_t episode = 0;
  std::string captureError;
  try {
    episode = dependencies_.capture({.event = event, .ownerUserId = config_.ownerUserId, .lang = config_.lang});
  }
  catch (const std::exception& error) {
    captureError = error.what();
  }
  if (!captureError.empty() || episode <= 0) {
    int64_t attempts = -1;
    {
      const std::scoped_lock lock(dependencies_.graph->mutex());
      attempts = dependencies_.repository->noteEncounterAttempt(dependencies_.graph->handle(),
                                                                {.eventId = event.eventId, .at = now});
    }
    LOG_WARN << "Maintenance consumer: capture failed for " << event.eventId << " (attempt " << attempts
             << (captureError.empty() ? std::string("; rejected") : ": " + captureError) << ")";
    if (attempts >= config_.poisonMaxAttempts) {
      const std::scoped_lock lock(dependencies_.graph->mutex());
      if (dependencies_.repository->markEncounterDeadLettered(dependencies_.graph->handle(),
                                                              {.eventId = event.eventId, .at = now})) {
        LOG_ERROR << "Maintenance consumer: dead-lettered poison window " << event.eventId;
        return MaintenanceDisposition::Term;
      }
    }
    return MaintenanceDisposition::Nak;
  }
  const std::scoped_lock lock(dependencies_.graph->mutex());
  if (!dependencies_.repository->markEncounterDispatched(dependencies_.graph->handle(),
                                                         {.eventId = event.eventId, .at = now}))
    throw std::runtime_error("maintenance receipt settle failed");
  return MaintenanceDisposition::Ack;
}

void MaintenanceEndedConsumer::settleDelivery(const MaintenanceDelivery& delivery)
{
  const auto& settlement = delivery.settlement;
  MaintenanceDisposition disposition = MaintenanceDisposition::Nak;
  if (delivery.alive) {
    try {
      disposition = settlePayload(delivery.payload);
    }
    catch (const std::exception& error) {
      LOG_WARN << "Maintenance consumer: redelivering (" << error.what() << ")";
      disposition = MaintenanceDisposition::Nak;
    }
  }
  if (disposition == MaintenanceDisposition::Term) {
    if (settlement.term)
      settlement.term();
  }
  else if (disposition == MaintenanceDisposition::Ack) {
    if (settlement.ack)
      settlement.ack();
  }
  else if (settlement.nak) {
    settlement.nak();
  }
}

bool MaintenanceEndedConsumer::trySubscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = true,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kDefaultMaxAckPending,
       .handler = [this, lifecycle = lifecycle_](const NatsBus::DurableMessage& message,
                                                 NatsBus::DurableSettlement settlement) {
         const auto nak = settlement.nak;
         try {
           strand_.post([this, lifecycle, payload = std::string(message.payload),
                         settlement = std::move(settlement)]() {
             const MaintenanceLifecycleGuard guard(lifecycle);
             settleDelivery({.alive = guard.alive(), .payload = payload, .settlement = settlement});
           });
         }
         catch (const std::exception& error) {
           LOG_WARN << "Maintenance consumer: redelivering, not scheduled (" << error.what() << ")";
           if (nak)
             nak();
         }
       }});
  if (!subscription)
    return false;
  subscription_ = subscription;
  return true;
}

void MaintenanceEndedConsumer::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(5.0, [this, lifecycle = lifecycle_]() {
    const MaintenanceLifecycleGuard guard(lifecycle);
    if (!guard.alive() || subscription_.has_value() || dependencies_.bus == nullptr)
      return;
    if (trySubscribe())
      LOG_INFO << "Maintenance consumer: durable consumer connected";
  });
}
