#include "module-feed.hxx"

#include <config/config-service.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <text/json-util.hxx>

#include <drogon/drogon.h>
#include <json/value.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <utility>

namespace
{
constexpr std::string_view kSettledField = "settled";
constexpr std::string_view kVersionField = "version";
constexpr std::string_view kEpochField = "epoch";
constexpr const char* kStateFileKey = "modules.state_file";
constexpr const char* kDefaultStateFile = "database/module-state.json";

struct InFlight
{
  explicit InFlight(std::atomic<int64_t>& counter) : counter_(counter)
  {
    counter_.fetch_add(1, std::memory_order_acq_rel);
  }
  ~InFlight() { counter_.fetch_sub(1, std::memory_order_acq_rel); }
  InFlight(const InFlight&) = delete;
  InFlight& operator=(const InFlight&) = delete;

private:
  std::atomic<int64_t>& counter_;
};

std::optional<int64_t> epochMintedAt(std::string_view epoch)
{
  const auto dash = epoch.find('-');
  if (dash == std::string_view::npos || dash == 0)
    return std::nullopt;
  int64_t minted = 0;
  const auto digits = epoch.substr(0, dash);
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), minted);
  if (error != std::errc{} || end != digits.data() + digits.size())
    return std::nullopt;
  return minted;
}

bool supersededEpoch(std::string_view incoming, std::string_view current)
{
  const auto incomingAt = epochMintedAt(incoming);
  const auto currentAt = epochMintedAt(current);
  return incomingAt && currentAt && *incomingAt < *currentAt;
}

void settleWith(const std::function<void()>& outcome)
{
  if (outcome)
    outcome();
}
}

ModuleFeed::Config ModuleFeed::defaults(std::string_view service)
{
  std::string stateFile = ConfigService::getString(kStateFileKey);
  return {.stream = nats_subject::kSettingsModuleStream,
          .subject = nats_subject::kSettingsModule,
          .durable = "argus-" + std::string(service) + "-modules",
          .stateFile = stateFile.empty() ? std::string(kDefaultStateFile) : std::move(stateFile),
          .maxDeliver = 10,
          .retrySeconds = 5.0,
          .bootAttempts = 12};
}

ModuleFeed::ModuleFeed(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)), config_(std::move(config))
{
}

ModuleFeed::~ModuleFeed()
{
  requestStop();
  if (connector_.joinable())
    connector_.join();
}

void ModuleFeed::restore()
{
  if (dependencies_.gate == nullptr)
    return;
  if (const auto flags = module_gate::loadStateFile(config_.stateFile))
    dependencies_.gate->apply(*flags);
}

bool ModuleFeed::applyAuthoritative(const Snapshot& snapshot)
{
  if (dependencies_.gate == nullptr)
    return false;
  std::scoped_lock lock(applyMutex_);
  if (!snapshot.epoch.empty() && snapshot.epoch != epoch_) {
    if (!epoch_.empty() && supersededEpoch(snapshot.epoch, epoch_))
      return false;
    epoch_ = snapshot.epoch;
    version_ = 0;
  }
  if (snapshot.version > 0 && snapshot.version < version_)
    return false;
  version_ = std::max(version_, snapshot.version);
  dependencies_.gate->apply(snapshot.flags);
  if (!module_gate::saveStateFile(config_.stateFile, dependencies_.gate->known()))
    LOG_WARN << "Modules: could not keep the last known state at " << config_.stateFile;
  return true;
}

int64_t ModuleFeed::version() const
{
  std::scoped_lock lock(applyMutex_);
  return version_;
}

std::string ModuleFeed::epoch() const
{
  std::scoped_lock lock(applyMutex_);
  return epoch_;
}

ModuleFeedDisposition ModuleFeed::handle(std::string_view body)
{
  const InFlight guard(inFlight_);
  const Json::Value json = json_util::fromString(std::string(body));
  if (!json.isObject())
    return ModuleFeedDisposition::Refused;
  const Json::Value& settled = json[std::string(kSettledField)];
  if ((settled.isBool() && !settled.asBool()) ||
      !json.isMember(std::string(module_gate::kModulesField)))
    return ModuleFeedDisposition::Ignored;
  const auto flags = module_gate::parseEnabledSet(body);
  if (!flags)
    return ModuleFeedDisposition::Refused;
  const Json::Value& version = json[std::string(kVersionField)];
  const int64_t at = version.isIntegral() ? version.asInt64() : 0;
  const Json::Value& epoch = json[std::string(kEpochField)];
  return applyAuthoritative({.flags = *flags, .version = at, .epoch = epoch.isString() ? epoch.asString() : std::string()})
             ? ModuleFeedDisposition::Applied
             : ModuleFeedDisposition::Ignored;
}

void ModuleFeed::start()
{
  if (connector_.joinable())
    return;
  connecting_.store(true, std::memory_order_release);
  connector_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ModuleFeed::run(const std::stop_token& stop)
{
  int bootAttempts = dependencies_.bootRead ? config_.bootAttempts : 0;
  bool subscribed = !dependencies_.bus;
  bool warned = false;
  while (!stop.stop_requested()) {
    if (bootAttempts > 0) {
      if (const auto snapshot = dependencies_.bootRead()) {
        applyAuthoritative(*snapshot);
        bootAttempts = 0;
      }
      else if (--bootAttempts == 0) {
        LOG_WARN << "Modules: settings did not answer the enabled set; keeping the last known state";
      }
    }
    if (!subscribed) {
      subscribed = subscribe();
      if (!subscribed && !warned)
        LOG_WARN << "Modules: " << config_.subject << " not ready; retrying";
      warned = warned || !subscribed;
    }
    if (subscribed && bootAttempts == 0)
      break;
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, std::chrono::duration<double>(config_.retrySeconds),
                   [] { return false; });
  }
  if (stop.stop_requested())
    requestStop();
  connecting_.store(false, std::memory_order_release);
}

bool ModuleFeed::subscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = false,
       .deliverLastPerSubject = true,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kOrderedMaxAckPending,
       .handler = [this](const NatsBus::DurableMessage& message,
                         const NatsBus::DurableSettlement& settlement) {
         const auto outcome = handle(message.payload);
         settleWith(outcome == ModuleFeedDisposition::Refused ? settlement.term
                                                              : settlement.ack);
       }});
  if (!subscription)
    return false;
  {
    std::scoped_lock lock(mutex_);
    subscription_ = subscription;
  }
  LOG_INFO << "Modules: durable " << config_.durable << " connected on " << config_.subject;
  return true;
}

void ModuleFeed::requestStop()
{
  connector_.request_stop();
  std::optional<uint64_t> subscription;
  {
    std::scoped_lock lock(mutex_);
    subscription.swap(subscription_);
  }
  if (subscription.has_value() && dependencies_.bus)
    dependencies_.bus->unsubscribe(*subscription);
}

bool ModuleFeed::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0 &&
         !connecting_.load(std::memory_order_acquire);
}

namespace module_gate
{
std::unique_ptr<ModuleFeed> install(const ModuleGateInstall& input)
{
  auto feed = std::make_unique<ModuleFeed>(
      ModuleFeed::Dependencies{.bus = input.bus,
                               .gate = &moduleGate(),
                               .bootRead = settingsBootRead()},
      ModuleFeed::defaults(input.service));
  feed->restore();
  drogon::app().registerBeginningAdvice([raw = feed.get()]() { raw->start(); });
  shutdown_signal::onStop(shutdown_signal::drainOf(*feed, "modules"));
  return feed;
}
}
