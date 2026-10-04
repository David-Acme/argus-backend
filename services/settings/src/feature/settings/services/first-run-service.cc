#include "first-run-service.hxx"

#include <feature/settings/services/profile-planner.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace
{
constexpr std::chrono::seconds kFirstDelay{5};

std::int64_t now()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string joined(const std::vector<std::string>& names)
{
  std::string text;
  for (const auto& name : names) {
    if (!text.empty())
      text += ", ";
    text += name;
  }
  return text.empty() ? "none" : text;
}

bool marked(const std::optional<OwnerCatalog>& catalog)
{
  return catalog && catalog->profile && catalog->profile->origin != ProfileOrigin::None;
}
}

FirstRunService::FirstRunService(FirstRunInput input)
    : gateway_(input.gateway), catalog_(std::move(input.catalog)), hardware_(input.hardware), config_(input.config)
{
}

FirstRunService::~FirstRunService()
{
  requestStop();
}

void FirstRunService::start()
{
  if (!config_.enabled || !catalog_ || running_.exchange(true))
    return;
  worker_ = std::jthread([this](const std::stop_token& stop) { loop(stop); });
}

void FirstRunService::loop(const std::stop_token& stop)
{
  std::chrono::seconds delay = kFirstDelay;
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(waitMutex_);
      if (wake_.wait_for(lock, stop, delay, [] { return false; }) || stop.stop_requested())
        break;
    }
    const auto pass = runOnce();
    if (pass.waiting.empty())
      break;
    delay = config_.interval;
  }
  running_ = false;
}

FirstRunPass FirstRunService::runOnce()
{
  FirstRunPass pass;
  if (!catalog_)
    return pass;
  const auto recommended = settings_profile::recommend(*catalog_, hardware_).profile;
  const auto* profile = catalog_->find(recommended);
  if (profile == nullptr)
    return pass;

  std::vector<std::string> owners;
  for (const auto& owner : profile->owners)
    if (std::ranges::find(settled_, owner.owner) == settled_.end())
      owners.push_back(owner.owner);
  if (owners.empty())
    return pass;

  const auto catalogs = gateway_.catalogsOf(owners);
  std::vector<OwnerPlan> plans;
  for (const auto& catalog : catalogs) {
    if (!catalog.reachable) {
      pass.waiting.push_back(catalog.service);
      continue;
    }
    if (!catalog.profile) {
      LOG_INFO << "Settings first run: " << catalog.service << " predates profile markers; left as it is";
      pass.settled.push_back(catalog.service);
      continue;
    }
    if (catalog.profile->origin != ProfileOrigin::None) {
      pass.settled.push_back(catalog.service);
      continue;
    }
    auto plan = settings_profile::firstRunPlan(*profile, catalog);
    plan.marker = ProfileMarker{.id = profile->id, .origin = ProfileOrigin::Recommended, .appliedAt = now(), .keys = {}};
    plans.push_back(std::move(plan));
  }
  for (const auto& owner : owners)
    if (std::ranges::find(catalogs, owner, &OwnerCatalog::service) == catalogs.end())
      pass.settled.push_back(owner);

  if (!plans.empty()) {
    ProfileApplication application(std::move(plans));
    for (auto writes = application.pendingWrites(); !writes.empty(); writes = application.pendingWrites())
      application.record(gateway_.write(writes));
    if (auto writes = application.markerWrites(); !writes.empty())
      application.record(gateway_.write(writes));
    for (const auto& result : application.results()) {
      if (!marked(result.catalog)) {
        pass.waiting.push_back(result.service);
        continue;
      }
      std::vector<std::string> applied;
      for (const auto& key : result.results)
        if (key.status == ProfileKeyStatus::Applied)
          applied.push_back(key.key);
      LOG_INFO << "Settings first run: applied the recommended profile " << profile->id << " to " << result.service
               << " [" << joined(applied) << "]";
      pass.settled.push_back(result.service);
    }
  }

  settled_.insert(settled_.end(), pass.settled.begin(), pass.settled.end());
  if (!pass.waiting.empty())
    LOG_INFO << "Settings first run: waiting for " << joined(pass.waiting);
  return pass;
}

void FirstRunService::requestStop()
{
  if (worker_.joinable()) {
    worker_.request_stop();
    wake_.notify_all();
  }
}

bool FirstRunService::drained() const
{
  return !running_.load();
}
