#include "module-feed.hxx"

#include <config/config-service.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <text/json-util.hxx>

#include <drogon/drogon.h>
#include <json/value.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace
{
constexpr std::string_view kSettledField = "settled";
constexpr std::string_view kVersionField = "version";
constexpr std::string_view kEpochField = "epoch";
constexpr int kMaxBackoffDoublings = 6;
constexpr double kMaxSubscribeRetrySeconds = 300.0;
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
  return apply(snapshot).applied;
}

ModuleFeed::Application ModuleFeed::apply(const Snapshot& snapshot)
{
  if (dependencies_.gate == nullptr)
    return {};
  std::scoped_lock lock(applyMutex_);
  bool epochChanged = false;
  if (!snapshot.epoch.empty() && snapshot.epoch != epoch_) {
    epochChanged = !epoch_.empty();
    epoch_ = snapshot.epoch;
    version_ = 0;
  }
  if (snapshot.version > 0 && snapshot.version < version_)
    return {};
  version_ = std::max(version_, snapshot.version);
  dependencies_.gate->apply(snapshot.flags);
  if (!module_gate::saveStateFile(config_.stateFile, dependencies_.gate->known()))
    LOG_WARN << "Modules: could not keep the last known state at " << config_.stateFile;
  return {.applied = true, .epochChanged = epochChanged};
}

void ModuleFeed::repull()
{
  if (!dependencies_.bootRead)
    return;
  if (const auto snapshot = dependencies_.bootRead())
    applyAuthoritative(*snapshot);
  else
    LOG_WARN << "Modules: settings did not answer the enabled set after the epoch changed";
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
  const auto result = apply({.flags = *flags, .version = at, .epoch = epoch.isString() ? epoch.asString() : std::string()});
  if (result.epochChanged)
    repull();
  return result.applied ? ModuleFeedDisposition::Applied : ModuleFeedDisposition::Ignored;
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
  int subscribeFailures = 0;
  auto nextSubscribe = std::chrono::steady_clock::now();
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
    if (!subscribed && std::chrono::steady_clock::now() >= nextSubscribe) {
      subscribed = subscribe();
      if (!subscribed) {
        const double delay = std::min(config_.retrySeconds * std::pow(2.0, std::min(subscribeFailures, kMaxBackoffDoublings)),
                                      kMaxSubscribeRetrySeconds);
        ++subscribeFailures;
        nextSubscribe = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                              std::chrono::duration<double>(delay));
        LOG_WARN << "Modules: durable " << config_.durable << " on " << config_.subject << " is not ready; retrying in "
                 << delay << " s";
      }
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
       .recreateOnPolicyChange = true,
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
