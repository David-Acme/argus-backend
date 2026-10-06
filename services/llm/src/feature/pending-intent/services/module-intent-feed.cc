#include "module-intent-feed.hxx"

#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>

#include <trantor/utils/Logger.h>

#include <chrono>
#include <utility>

namespace
{
constexpr std::string_view kEnabledKind = "enabled";
constexpr std::string_view kModuleKind = "module";
constexpr std::string_view kActive = "active";
constexpr std::string_view kInstallJob = "install";

bool stringIs(const Json::Value& value, std::string_view expected)
{
  return value.isString() && value.asString() == expected;
}

void decodeEnabled(const Json::Value& json, std::vector<ModuleSignal>& signals)
{
  for (const auto& module : json["modules"]) {
    if (!module["id"].isString() || !module["enabled"].isBool() || !module["enabled"].asBool())
      continue;
    if (module["lifecycle"].isString() && !stringIs(module["lifecycle"], kActive))
      continue;
    signals.push_back({.kind = ModuleSignal::Kind::Activated, .module = module["id"].asString(), .reason = {}});
  }
}

void decodeModule(const Json::Value& json, std::vector<ModuleSignal>& signals)
{
  const Json::Value& module = json["module"];
  const Json::Value& job = module["job"];
  if (!module["id"].isString() || !job.isObject())
    return;
  if (job["kind"].isString() && !stringIs(job["kind"], kInstallJob))
    return;
  if (!stringIs(job["state"], "failed") && !stringIs(job["state"], "cancelled"))
    return;
  signals.push_back({.kind = ModuleSignal::Kind::Failed,
                     .module = module["id"].asString(),
                     .reason = job["reason"].isString() ? job["reason"].asString() : job["state"].asString()});
}
}

ModuleIntentFeed::ModuleIntentFeed(std::shared_ptr<NatsBus> bus, PendingIntentService& service,
                                   ModuleIntentFeedConfig config)
    : bus_(std::move(bus)), service_(service), config_(std::move(config))
{
}

ModuleIntentFeed::~ModuleIntentFeed()
{
  requestStop();
  if (connector_.joinable())
    connector_.join();
}

ModuleIntentFeedConfig ModuleIntentFeed::defaults()
{
  return {.stream = std::string(nats_subject::kSettingsModuleStream),
          .subject = std::string(nats_subject::kSettingsModule),
          .durable = "argus-llm-intents",
          .maxDeliver = 10,
          .retrySeconds = 5.0};
}

std::vector<ModuleSignal> ModuleIntentFeed::decode(std::string_view payload)
{
  std::vector<ModuleSignal> signals;
  const Json::Value json = json_util::fromString(std::string(payload));
  if (!json.isObject())
    return signals;
  if (json["settled"].isBool() && !json["settled"].asBool())
    return signals;
  if (stringIs(json["kind"], kEnabledKind))
    decodeEnabled(json, signals);
  else if (stringIs(json["kind"], kModuleKind))
    decodeModule(json, signals);
  return signals;
}

bool ModuleIntentFeed::subscribe()
{
  const auto subscription = bus_->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = true,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kOrderedMaxAckPending,
       .handler = [this](const NatsBus::DurableMessage& message, const NatsBus::DurableSettlement& settlement) {
         inFlight_.fetch_add(1, std::memory_order_acq_rel);
         for (auto& signal : decode(message.payload)) {
           if (signal.kind == ModuleSignal::Kind::Activated)
             service_.postActivated(std::move(signal.module));
           else
             service_.postFailed({.module = std::move(signal.module), .reason = std::move(signal.reason)});
         }
         settlement.ack();
         inFlight_.fetch_sub(1, std::memory_order_acq_rel);
       }});
  if (!subscription)
    return false;
  {
    const std::scoped_lock lock(mutex_);
    subscription_ = subscription;
  }
  LOG_INFO << "argus-llm: durable " << config_.durable << " follows " << config_.subject;
  return true;
}

void ModuleIntentFeed::start()
{
  if (!bus_ || connector_.joinable())
    return;
  connecting_.store(true, std::memory_order_release);
  connector_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ModuleIntentFeed::run(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    if (subscribe())
      break;
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, std::chrono::duration<double>(config_.retrySeconds), [] { return false; });
  }
  connecting_.store(false, std::memory_order_release);
}

void ModuleIntentFeed::requestStop()
{
  connector_.request_stop();
  wake_.notify_all();
  std::optional<uint64_t> subscription;
  {
    const std::scoped_lock lock(mutex_);
    subscription.swap(subscription_);
  }
  if (subscription && bus_)
    bus_->unsubscribe(*subscription);
}

bool ModuleIntentFeed::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0 && !connecting_.load(std::memory_order_acquire);
}
