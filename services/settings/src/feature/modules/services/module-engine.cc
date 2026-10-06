#include "module-engine.hxx"

#include <errors/response-exception.hxx>
#include <feature/modules/module-errors.hxx>
#include <feature/modules/services/module-resolver.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <format>
#include <random>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace
{
constexpr int kMaxStepsPerTick = 8;
constexpr double kRateWeight = 0.3;
constexpr std::int64_t kMillisPerSecond = 1000;

std::string mintEpoch(std::int64_t bootMs)
{
  std::random_device entropy;
  const std::uint64_t salt = (static_cast<std::uint64_t>(entropy()) << 32U) | entropy();
  return std::format("{}-{:016x}", bootMs, salt);
}

bool settledReading(const ComponentStatus& status, OwnerReach reach)
{
  return reach == OwnerReach::Unsupported || (reach == OwnerReach::Answered && status.state == ComponentState::Installed);
}

bool readyReading(const ComponentStatus& status, OwnerReach reach)
{
  return reach == OwnerReach::Unsupported ||
         (reach == OwnerReach::Answered && status.state == ComponentState::Installed && status.ready);
}

class SyncTransaction
{
public:
  explicit SyncTransaction(drogon::orm::DbClientPtr db) : db_(std::move(db)) { db_->execSqlSync("BEGIN IMMEDIATE"); }

  ~SyncTransaction()
  {
    if (committed_)
      return;
    try {
      db_->execSqlSync("ROLLBACK");
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Modules: rollback failed: " << error.what();
    }
  }

  SyncTransaction(const SyncTransaction&) = delete;
  SyncTransaction& operator=(const SyncTransaction&) = delete;
  SyncTransaction(SyncTransaction&&) = delete;
  SyncTransaction& operator=(SyncTransaction&&) = delete;

  void commit()
  {
    db_->execSqlSync("COMMIT");
    committed_ = true;
  }

private:
  drogon::orm::DbClientPtr db_;
  bool committed_{false};
};
}

ModuleEngine::ModuleEngine(ModuleEngineInput input)
    : catalog_(std::move(input.catalog)),
      db_(std::move(input.db)),
      owners_(input.owners),
      events_(input.events),
      host_(std::move(input.host)),
      clock_(std::move(input.clock)),
      timing_(input.timing),
      stateRepository_(db_),
      jobRepository_(db_),
      auditRepository_(db_),
      purgeRepository_(db_)
{
  if (!db_ || !host_ || !clock_)
    throw std::invalid_argument("The module engine needs a database, a host probe and a clock");
  load();
}

ModuleEngine::~ModuleEngine()
{
  requestStop();
}

template <typename Body>
void ModuleEngine::transactLocked(Body&& body)
{
  try {
    SyncTransaction transaction(db_);
    body();
    transaction.commit();
  }
  catch (...) {
    reloadLocked();
    throw;
  }
}

void ModuleEngine::load()
{
  const std::scoped_lock lock(mutex_);
  bootMs_ = clock_();
  epoch_ = mintEpoch(bootMs_);
  for (const auto& job : jobRepository_.findOpen()) {
    if (jobRunning(job.state))
      static_cast<void>(jobRepository_.update(job.id, {.at = bootMs_,
                                                       .state = JobState::Queued,
                                                       .reason = std::nullopt,
                                                       .owner = std::nullopt,
                                                       .bytesDone = std::nullopt,
                                                       .bytesTotal = std::nullopt}));
    if (job.state == JobState::Paused)
      pendingStops_.insert(job.moduleId);
  }
  reloadLocked();
}

void ModuleEngine::reloadLocked()
{
  enabled_.clear();
  lifecycles_.clear();
  purgedAt_.clear();
  const auto states = stateRepository_.findAll();
  settled_ = !states.empty();
  for (const auto& state : states) {
    lifecycles_[state.moduleId] = state.lifecycle;
    purgedAt_[state.moduleId] = state.dataPurgedAt;
    if (state.lifecycle == ModuleLifecycle::Active)
      enabled_.insert(state.moduleId);
  }
  if (settled_)
    for (const auto& module : catalog_.modules)
      if (module.kind == ModuleKind::Core) {
        enabled_.insert(module.id);
        lifecycles_[module.id] = ModuleLifecycle::Active;
      }
  version_ = auditRepository_.enabledVersion();
  jobs_.clear();
  for (auto& job : jobRepository_.findLatestPerModule())
    jobs_[job.moduleId] = std::move(job);
}

std::set<std::string> ModuleEngine::ownersOf(const CatalogModule& module) const
{
  std::set<std::string> owners;
  for (const auto& id : module.components)
    if (const auto* component = catalog_.component(id))
      owners.insert(component->owner);
  return owners;
}

std::set<std::string> ModuleEngine::allOwners() const
{
  std::set<std::string> owners;
  for (const auto& component : catalog_.components)
    owners.insert(component.owner);
  return owners;
}

void ModuleEngine::refresh(const std::set<std::string>& owners)
{
  std::vector<std::pair<std::string, std::vector<ComponentSpec>>> requests;
  for (const auto& owner : owners) {
    std::vector<ComponentSpec> specs;
    for (const auto& component : catalog_.components)
      if (component.owner == owner)
        specs.push_back(component.spec);
    if (!specs.empty())
      requests.emplace_back(owner, std::move(specs));
  }
  std::vector<OwnerStates> answers(requests.size());
  {
    std::vector<std::jthread> workers;
    workers.reserve(requests.size());
    for (const auto index : std::views::iota(std::size_t{0}, requests.size()))
      workers.emplace_back([this, &requests, &answers, index] {
        answers[index] = owners_.states(requests[index].first, requests[index].second);
      });
  }
  const std::scoped_lock lock(mutex_);
  for (const auto index : std::views::iota(std::size_t{0}, requests.size())) {
    const auto& answer = answers[index];
    for (const auto& spec : requests[index].second) {
      auto& reading = readings_[spec.id];
      reading.reach = answer.reach;
      if (answer.reach == OwnerReach::Unreachable)
        continue;
      const auto status = std::ranges::find(answer.states, spec.id, &ComponentStatus::id);
      if (answer.reach == OwnerReach::Answered && status != answer.states.end()) {
        reading.status = *status;
      }
      else {
        reading.status = {.id = spec.id,
                          .state = ComponentState::Installed,
                          .bytesPresent = spec.totalBytes(),
                          .bytesTotal = spec.totalBytes(),
                          .ready = true,
                          .hostCommand = {},
                          .reason = {}};
        if (answer.reach == OwnerReach::Answered)
          reading.reach = OwnerReach::Unsupported;
      }
      reading.known = true;
    }
  }
  refreshedAt_ = clock_();
}

bool ModuleEngine::seedReadyLocked() const
{
  return std::ranges::all_of(catalog_.components, [this](const CatalogComponent& component) {
    const auto reading = readings_.find(component.spec.id);
    return reading != readings_.end() && reading->second.known && reading->second.reach != OwnerReach::Unreachable;
  });
}

void ModuleEngine::seedLocked(Outbox& outbox)
{
  const auto now = clock_();
  std::set<std::string> enabled;
  transactLocked([&] {
  for (const auto& module : catalog_.modules) {
    bool adopt = module.kind == ModuleKind::Core;
    if (module.kind == ModuleKind::Available)
      adopt = std::ranges::all_of(module.components, [this](const std::string& id) {
        const auto reading = readings_.find(id);
        if (reading == readings_.end() || !reading->second.known || reading->second.reach == OwnerReach::Unreachable)
          return true;
        return settledReading(reading->second.status, reading->second.reach);
      });
    static_cast<void>(stateRepository_.upsert({.moduleId = module.id,
                             .lifecycle = adopt ? ModuleLifecycle::Active : ModuleLifecycle::NotInstalled,
                             .dataPurgedAt = std::nullopt,
                             .at = now}));
    if (adopt) {
      static_cast<void>(auditRepository_.create(
          {.moduleId = module.id, .action = ModuleAuditAction::Adopted, .userId = 0, .detail = "seed", .at = now}));
      enabled.insert(module.id);
    }
  }
  });
  for (const auto& module : catalog_.modules)
    lifecycles_[module.id] = enabled.contains(module.id) ? ModuleLifecycle::Active : ModuleLifecycle::NotInstalled;
  enabled_ = std::move(enabled);
  settled_ = true;
  version_ = auditRepository_.enabledVersion();
  outbox.enabled = true;
  LOG_INFO << "Modules: the enabled set is settled with " << enabled_.size() << " modules";
}

std::optional<ModuleJobSchema> ModuleEngine::openJobLocked(const std::string& moduleId) const
{
  const auto found = jobs_.find(moduleId);
  if (found == jobs_.end() || jobTerminal(found->second.state))
    return std::nullopt;
  return found->second;
}

std::optional<ModuleJobSchema> ModuleEngine::nextJobLocked(Outbox& outbox)
{
  std::vector<ModuleJobSchema> open;
  for (const auto& [id, job] : jobs_)
    if (!jobTerminal(job.state) && job.state != JobState::Paused)
      open.push_back(job);
  std::ranges::sort(open, {}, &ModuleJobSchema::id);
  for (const auto& job : open)
    if (jobRunning(job.state))
      return job;
  for (const auto& job : open) {
    const auto* module = catalog_.module(job.moduleId);
    if (module == nullptr) {
      changeJobLocked({.jobId = job.id,
                       .moduleId = job.moduleId,
                       .update = {.at = clock_(),
                                  .state = JobState::Failed,
                                  .reason = std::string(job_reason::kDependencyFailed),
                                  .owner = std::nullopt,
                                  .bytesDone = std::nullopt,
                                  .bytesTotal = std::nullopt}},
                      outbox);
      continue;
    }
    bool waiting = false;
    bool broken = false;
    for (const auto& required : module->required) {
      if (enabled_.contains(required))
        continue;
      if (openJobLocked(required))
        waiting = true;
      else
        broken = true;
    }
    if (broken) {
      changeJobLocked({.jobId = job.id,
                       .moduleId = job.moduleId,
                       .update = {.at = clock_(),
                                  .state = JobState::Failed,
                                  .reason = std::string(job_reason::kDependencyFailed),
                                  .owner = std::nullopt,
                                  .bytesDone = std::nullopt,
                                  .bytesTotal = std::nullopt}},
                      outbox);
      auditLocked({.moduleId = job.moduleId, .userId = job.requestedBy}, ModuleAuditAction::Failed,
                  job_reason::kDependencyFailed);
      continue;
    }
    if (!waiting)
      return job;
  }
  return std::nullopt;
}

ModuleEngine::StepPlan ModuleEngine::planLocked(const ModuleJobSchema& job, const CatalogModule& module)
{
  return job.kind == JobKind::Install ? planInstallLocked(job, module) : planUninstallLocked(job, module);
}

std::vector<ModuleEngine::OwnerCall> ModuleEngine::removalsLocked(const CatalogModule& module) const
{
  std::vector<OwnerCall> calls;
  for (const auto& id : module.components) {
    const auto* component = catalog_.component(id);
    if (component == nullptr || component->spec.source != ComponentSource::Download)
      continue;
    const bool shared = std::ranges::any_of(catalog_.modules, [this, &id, &module](const CatalogModule& other) {
      return other.id != module.id && enabled_.contains(other.id) &&
             std::ranges::find(other.components, id) != other.components.end();
    });
    const auto reading = readings_.find(id);
    const bool absent = reading != readings_.end() && reading->second.known &&
                        reading->second.reach == OwnerReach::Answered && reading->second.status.bytesPresent == 0;
    if (!shared && !absent)
      calls.push_back({.owner = component->owner, .spec = component->spec});
  }
  return calls;
}

ModuleEngine::StepPlan ModuleEngine::planUninstallLocked(const ModuleJobSchema& job, const CatalogModule& module)
{
  StepPlan plan;
  const auto now = clock_();
  const auto moveTo = [&plan, &job, now](JobState state) {
    plan.transition = Transition{.jobId = job.id,
                                 .from = job.state,
                                 .update = {.at = now,
                                            .state = state,
                                            .reason = std::string(),
                                            .owner = std::string(),
                                            .bytesDone = std::nullopt,
                                            .bytesTotal = std::nullopt}};
  };
  switch (job.state) {
  case JobState::Queued:
    moveTo(JobState::Removing);
    break;
  case JobState::Removing:
    plan.removes = removalsLocked(module);
    if (job.kind == JobKind::Uninstall)
      plan.finishUninstall = true;
    moveTo(job.kind == JobKind::Uninstall ? JobState::Done : JobState::Purging);
    break;
  case JobState::Purging: {
    const auto purged = purgeRepository_.purgedOwners(module.id);
    for (const auto& owner : module.dataOwners)
      if (!purged.contains(owner))
        plan.purges.push_back(owner);
    plan.finishUninstall = true;
    moveTo(JobState::Done);
    break;
  }
  default:
    break;
  }
  return plan;
}

ModuleEngine::StepPlan ModuleEngine::planInstallLocked(const ModuleJobSchema& job, const CatalogModule& module)
{
  StepPlan plan;
  const auto now = clock_();
  const auto moveTo = [&plan, &job, now](JobState state, std::optional<std::string> reason) {
    plan.transition = Transition{.jobId = job.id,
                                 .from = job.state,
                                 .update = {.at = now,
                                            .state = state,
                                            .reason = std::move(reason),
                                            .owner = std::nullopt,
                                            .bytesDone = std::nullopt,
                                            .bytesTotal = std::nullopt}};
  };

  bool unknown = false;
  bool allSettled = true;
  bool allReady = true;
  const ComponentReading* failed = nullptr;
  bool hostOnly = false;
  std::vector<OwnerCall> missing;
  for (const auto& id : module.components) {
    const auto* component = catalog_.component(id);
    const auto found = readings_.find(id);
    if (component == nullptr || found == readings_.end() || !found->second.known ||
        found->second.reach == OwnerReach::Unreachable) {
      unknown = true;
      allSettled = false;
      allReady = false;
      continue;
    }
    const auto& reading = found->second;
    allSettled = allSettled && settledReading(reading.status, reading.reach);
    allReady = allReady && readyReading(reading.status, reading.reach);
    if (reading.reach != OwnerReach::Answered || reading.status.state == ComponentState::Installed)
      continue;
    if (component->spec.source == ComponentSource::Provisioned)
      hostOnly = true;
    else if (reading.status.state == ComponentState::Failed)
      failed = &reading;
    else if (reading.status.state != ComponentState::Installing)
      missing.push_back({.owner = component->owner, .spec = component->spec});
  }

  switch (job.state) {
  case JobState::Queued:
    moveTo(JobState::Checking, std::string());
    break;
  case JobState::Checking:
    if (unknown) {
      if (job.reason != job_reason::kOwnerUnreachable)
        plan.transition = Transition{.jobId = job.id,
                                     .from = job.state,
                                     .update = {.at = now,
                                                .state = std::nullopt,
                                                .reason = std::string(job_reason::kOwnerUnreachable),
                                                .owner = std::nullopt,
                                                .bytesDone = std::nullopt,
                                                .bytesTotal = std::nullopt}};
      break;
    }
    if (hardwareLocked(module).verdict == HardwareVerdict::Insufficient) {
      moveTo(JobState::Failed, std::string(job_reason::kHardwareInsufficient));
      break;
    }
    if (hostOnly) {
      moveTo(JobState::Failed, std::string(job_reason::kHostOnly));
      break;
    }
    if (allSettled) {
      moveTo(JobState::Verifying, std::string());
      break;
    }
    plan.installs = std::move(missing);
    moveTo(JobState::Downloading, std::string());
    break;
  case JobState::Downloading:
    if (failed != nullptr) {
      for (const auto& id : module.components)
        if (const auto* component = catalog_.component(id);
            component != nullptr && component->spec.source == ComponentSource::Download)
          plan.cancels.push_back({.owner = component->owner, .spec = component->spec});
      moveTo(JobState::Failed, failed->status.reason.empty() ? std::string("network") : failed->status.reason);
      break;
    }
    if (allSettled) {
      moveTo(JobState::Verifying, std::string());
      break;
    }
    plan.installs = std::move(missing);
    break;
  case JobState::Verifying:
    if (allSettled)
      moveTo(JobState::Activating, std::string());
    else if (!unknown)
      moveTo(JobState::Downloading, std::string());
    break;
  case JobState::Activating:
    plan.activate = true;
    moveTo(JobState::HealthCheck, std::string());
    break;
  case JobState::HealthCheck:
    if (allReady)
      plan.transition = Transition{.jobId = job.id,
                                   .from = job.state,
                                   .update = {.at = now,
                                              .state = JobState::Done,
                                              .reason = std::string(),
                                              .owner = std::nullopt,
                                              .bytesDone = std::max(job.bytesDone, job.bytesTotal),
                                              .bytesTotal = std::nullopt}};
    else if (now - job.stateSince > std::chrono::duration_cast<std::chrono::milliseconds>(timing_.healthTimeout).count()) {
      plan.rollback = true;
      moveTo(JobState::Failed, std::string(job_reason::kHealthCheckFailed));
    }
    break;
  default:
    break;
  }
  return plan;
}

void ModuleEngine::auditLocked(const ModuleCommand& command, ModuleAuditAction action, const std::string& detail)
{
  const auto entry = auditRepository_.create(
      {.moduleId = command.moduleId, .action = action, .userId = command.userId, .detail = detail, .at = clock_()});
  if (changesEnabledSet(action))
    version_ = entry.id;
}

ModuleLifecycle ModuleEngine::lifecycleLocked(const std::string& moduleId) const
{
  const auto found = lifecycles_.find(moduleId);
  return found == lifecycles_.end() ? ModuleLifecycle::NotInstalled : found->second;
}

void ModuleEngine::setLifecycleLocked(const ModuleCommand& command, ModuleLifecycle lifecycle, ModuleAuditAction action)
{
  static_cast<void>(stateRepository_.upsert(
      {.moduleId = command.moduleId, .lifecycle = lifecycle, .dataPurgedAt = std::nullopt, .at = clock_()}));
  auditLocked(command, action, {});
  lifecycles_[command.moduleId] = lifecycle;
  if (lifecycle == ModuleLifecycle::Active)
    enabled_.insert(command.moduleId);
  else
    enabled_.erase(command.moduleId);
}

void ModuleEngine::changeJobLocked(const JobChange& change, Outbox& outbox)
{
  const auto& moduleId = change.moduleId;
  const auto updated = jobRepository_.update(change.jobId, change.update);
  if (!updated)
    return;
  jobs_[moduleId] = *updated;
  const auto* module = catalog_.module(moduleId);
  if (module == nullptr)
    return;
  const auto view = jobViewLocked(*updated);
  if (throttle_.admit({.jobId = updated->id, .state = updated->state, .progress = view.progress, .nowMs = clock_()})) {
    auto moduleView = viewLocked(*module);
    moduleView.job = view;
    outbox.modules.push_back(std::move(moduleView));
  }
  if (jobTerminal(updated->state))
    rates_.erase(updated->id);
}

void ModuleEngine::progressLocked(ModuleJobSchema& job, const CatalogModule& module, Outbox& outbox)
{
  const auto present = std::min(presentBytesLocked(module), job.bytesTotal);
  if (present <= job.bytesDone)
    return;
  const auto now = clock_();
  auto& rate = rates_[job.id];
  if (rate.atMs > 0 && now > rate.atMs) {
    const double instant =
        static_cast<double>(present - rate.bytes) * kMillisPerSecond / static_cast<double>(now - rate.atMs);
    rate.bytesPerSecond = rate.bytesPerSecond <= 0 ? instant : (1 - kRateWeight) * rate.bytesPerSecond + kRateWeight * instant;
  }
  rate.bytes = present;
  rate.atMs = now;
  changeJobLocked({.jobId = job.id,
                       .moduleId = job.moduleId,
                   .update = {.at = now,
                              .state = std::nullopt,
                              .reason = std::nullopt,
                              .owner = std::nullopt,
                              .bytesDone = present,
                              .bytesTotal = std::nullopt}},
                  outbox);
}

void ModuleEngine::applyLocked(const StepPlan& plan, const CatalogModule& module, Outbox& outbox)
{
  const auto current = jobs_.find(module.id);
  if (current == jobs_.end())
    return;
  if (plan.transition && current->second.state != plan.transition->from)
    return;
  try {
    const ModuleCommand command{.moduleId = module.id, .userId = current->second.requestedBy};
    transactLocked([&] {
    if (plan.activate)
      setLifecycleLocked(command, ModuleLifecycle::Active, ModuleAuditAction::Enabled);
    if (plan.rollback)
      setLifecycleLocked(command, ModuleLifecycle::Disabled, ModuleAuditAction::RolledBack);
    if (plan.finishUninstall && current->second.kind == JobKind::Purge) {
      const auto now = clock_();
      static_cast<void>(stateRepository_.upsert(
          {.moduleId = module.id, .lifecycle = ModuleLifecycle::NotInstalled, .dataPurgedAt = now, .at = now}));
      purgeRepository_.clear(module.id);
      auditLocked(command, ModuleAuditAction::Purged, {});
      lifecycles_[module.id] = ModuleLifecycle::NotInstalled;
      purgedAt_[module.id] = now;
      hasData_[module.id] = false;
    }
    else if (plan.finishUninstall) {
      const auto next = lifecycleLocked(module.id) == ModuleLifecycle::NotInstalled
                            ? ModuleLifecycle::NotInstalled
                            : ModuleLifecycle::UninstalledDataKept;
      setLifecycleLocked(command, next, ModuleAuditAction::Removed);
    }
    auto job = current->second;
    if (job.state == JobState::Downloading || job.state == JobState::Verifying)
      progressLocked(job, module, outbox);
    if (plan.transition) {
      changeJobLocked({.jobId = current->second.id, .moduleId = module.id, .update = plan.transition->update},
                      outbox);
      if (plan.transition->update.state == JobState::Failed)
        auditLocked(command, ModuleAuditAction::Failed, plan.transition->update.reason.value_or(""));
    }
    });
  }
  catch (const std::exception& error) {
    LOG_ERROR << "Modules: the job step for " << module.id << " was not saved: " << error.what();
    outbox.modules.clear();
    return;
  }
  if (plan.activate || plan.rollback || plan.finishUninstall)
    outbox.enabled = true;
}

std::optional<ModuleEngine::OwnerFailure> ModuleEngine::runOwnerCalls(StepPlan& plan, const CatalogModule& module)
{
  for (const auto& call : plan.installs)
    static_cast<void>(owners_.install(call.owner, call.spec));
  for (const auto& call : plan.cancels)
    static_cast<void>(owners_.cancel(call.owner, call.spec));
  for (const auto& call : plan.removes) {
    const auto reply = owners_.remove(call.owner, call.spec);
    if (reply.reach == OwnerReach::Unreachable)
      return OwnerFailure{.owner = call.owner, .reason = job_reason::kOwnerUnreachable};
    if (!reply.value)
      return OwnerFailure{.owner = call.owner, .reason = job_reason::kRemoveUnsupported};
    if (reply.value->bytesPresent > 0 && reply.value->state == ComponentState::Installed)
      return OwnerFailure{.owner = call.owner, .reason = job_reason::kRemoveFailed};
  }
  for (const auto& owner : plan.purges) {
    const auto reply = owners_.purgeData(owner, module.id);
    if (reply.reach == OwnerReach::Unreachable)
      return OwnerFailure{.owner = owner, .reason = job_reason::kOwnerUnreachable};
    if (!reply.value)
      return OwnerFailure{.owner = owner, .reason = job_reason::kPurgeUnsupported};
    if (!reply.value->purged)
      return OwnerFailure{.owner = owner, .reason = job_reason::kPurgeFailed};
    const std::scoped_lock lock(mutex_);
    purgeRepository_.create({.moduleId = module.id, .owner = owner, .at = clock_()});
  }
  return std::nullopt;
}

void ModuleEngine::tick()
{
  const std::scoped_lock tickLock(tickMutex_);
  const auto now = clock_();
  bool active = false;
  std::int64_t refreshedAt = 0;
  {
    const std::scoped_lock lock(mutex_);
    active = std::ranges::any_of(jobs_, [](const auto& entry) {
      return !jobTerminal(entry.second.state) && entry.second.state != JobState::Paused;
    });
    refreshedAt = refreshedAt_;
  }
  const auto interval = active ? timing_.pollInterval.count()
                               : std::chrono::duration_cast<std::chrono::milliseconds>(timing_.idleRefresh).count();
  if (refreshedAt == 0 || now - refreshedAt >= interval) {
    refresh(allOwners());
    std::vector<std::string> withData;
    for (const auto& module : catalog_.modules)
      if (!module.dataOwners.empty())
        withData.push_back(module.id);
    refreshData(withData);
  }

  Outbox outbox;
  std::vector<OwnerCall> stops;
  {
    const std::scoped_lock lock(mutex_);
    if (!settled_ && (seedReadyLocked() ||
                      now - bootMs_ >= std::chrono::duration_cast<std::chrono::milliseconds>(timing_.seedWait).count()))
      seedLocked(outbox);
    for (const auto& moduleId : pendingStops_)
      if (const auto* module = catalog_.module(moduleId))
        for (const auto& id : module->components)
          if (const auto* component = catalog_.component(id);
              component != nullptr && component->spec.source == ComponentSource::Download)
            stops.push_back({.owner = component->owner, .spec = component->spec});
    pendingStops_.clear();
  }
  for (const auto& stop : stops)
    static_cast<void>(owners_.cancel(stop.owner, stop.spec));

  for (int step = 0; step < kMaxStepsPerTick; ++step) {
    std::optional<ModuleJobSchema> job;
    {
      const std::scoped_lock lock(mutex_);
      if (settled_)
        job = nextJobLocked(outbox);
    }
    if (!job)
      break;
    const auto* module = catalog_.module(job->moduleId);
    if (job->state == JobState::Checking || job->state == JobState::Verifying)
      refresh(ownersOf(*module));
    StepPlan plan;
    {
      const std::scoped_lock lock(mutex_);
      const auto current = jobs_.find(job->moduleId);
      if (current == jobs_.end() || current->second.state != job->state)
        continue;
      plan = planLocked(current->second, *module);
    }
    if (const auto failure = runOwnerCalls(plan, *module); failure && plan.transition) {
      plan.finishUninstall = false;
      plan.transition->update.state = JobState::Failed;
      plan.transition->update.reason = failure->reason;
      plan.transition->update.owner = failure->owner;
    }
    if (!plan.installs.empty() || !plan.removes.empty())
      refresh(ownersOf(*module));
    const bool moved = plan.transition && plan.transition->update.state.has_value();
    {
      const std::scoped_lock lock(mutex_);
      applyLocked(plan, *module, outbox);
    }
    if (!moved)
      break;
  }
  flush(outbox);
}

void ModuleEngine::flush(const Outbox& outbox)
{
  const std::scoped_lock publishLock(publishMutex_);
  if (events_ == nullptr)
    return;
  ModuleStatesReply set;
  {
    const std::scoped_lock lock(mutex_);
    set = enabledSetLocked();
  }
  for (const auto& view : outbox.modules)
    static_cast<void>(events_->moduleChanged(view, set));
  if (set.settled && set.version > publishedVersion_ && events_->enabledChanged(set))
    publishedVersion_ = set.version;
}

ModuleStatesReply ModuleEngine::enabledSetLocked() const
{
  ModuleStatesReply set{.modules = {}, .version = version_, .settled = settled_, .epoch = epoch_};
  set.modules.reserve(catalog_.modules.size());
  for (const auto& module : catalog_.modules)
    set.modules.push_back({.id = module.id,
                           .enabled = module.kind == ModuleKind::Core || enabled_.contains(module.id),
                           .lifecycle = std::string(moduleLifecycleToString(
                               module.kind == ModuleKind::Core ? ModuleLifecycle::Active : lifecycleLocked(module.id))),
                           .dataPurgedAt = purgedAt_.contains(module.id) ? purgedAt_.at(module.id) : 0,
                           .roles = module.roles,
                           .name = {.es = module.name.es, .en = module.name.en},
                           .summary = {.es = module.summary.es, .en = module.summary.en},
                           .intro = module.intro,
                           .kind = std::string(moduleKindToString(module.kind))});
  return set;
}

ModuleStatesReply ModuleEngine::enabledSet() const
{
  const std::scoped_lock lock(mutex_);
  return enabledSetLocked();
}

std::vector<ComponentView> ModuleEngine::componentsLocked(const CatalogModule& module) const
{
  std::vector<ComponentView> views;
  for (const auto& id : module.components) {
    const auto* component = catalog_.component(id);
    if (component == nullptr)
      continue;
    ComponentView view{.component = component,
                       .status = {.id = id,
                                  .state = ComponentState::Missing,
                                  .bytesPresent = 0,
                                  .bytesTotal = component->spec.totalBytes(),
                                  .ready = false,
                                  .hostCommand = {},
                                  .reason = {}},
                       .reach = OwnerReach::Unreachable};
    if (const auto reading = readings_.find(id); reading != readings_.end() && reading->second.known) {
      view.status = reading->second.status;
      view.reach = reading->second.reach;
    }
    views.push_back(std::move(view));
  }
  return views;
}

std::int64_t ModuleEngine::presentBytesLocked(const CatalogModule& module) const
{
  std::int64_t present = 0;
  for (const auto& view : componentsLocked(module))
    present += std::min(view.status.bytesPresent, view.component->spec.totalBytes());
  return present;
}

std::int64_t ModuleEngine::remainingBytesLocked(const CatalogModule& module) const
{
  std::int64_t remaining = 0;
  for (const auto& view : componentsLocked(module))
    if (view.component->spec.source == ComponentSource::Download)
      remaining += std::max<std::int64_t>(view.component->spec.totalBytes() - view.status.bytesPresent, 0);
  return remaining;
}

HardwareAssessment ModuleEngine::hardwareLocked(const CatalogModule& module) const
{
  const auto host = host_();
  return assessHardware({.catalog = catalog_,
                         .module = module,
                         .enabled = enabled_,
                         .remainingBytes = remainingBytesLocked(module),
                         .host = host});
}

JobView ModuleEngine::jobViewLocked(const ModuleJobSchema& job) const
{
  JobView view{.job = job, .progress = 0, .bytesPerSecond = 0, .etaSeconds = std::nullopt};
  if (job.state == JobState::Done)
    view.progress = 1;
  else if (job.bytesTotal > 0)
    view.progress = std::min(1.0, static_cast<double>(job.bytesDone) / static_cast<double>(job.bytesTotal));
  const auto rate = rates_.find(job.id);
  if (rate != rates_.end() && rate->second.bytesPerSecond > 0 && !jobTerminal(job.state) &&
      job.state != JobState::Paused) {
    view.bytesPerSecond = static_cast<std::int64_t>(rate->second.bytesPerSecond);
    view.etaSeconds =
        static_cast<std::int64_t>(static_cast<double>(std::max<std::int64_t>(job.bytesTotal - job.bytesDone, 0)) /
                                  rate->second.bytesPerSecond);
  }
  return view;
}

ModuleView ModuleEngine::viewLocked(const CatalogModule& module) const
{
  ModuleView view{.module = &module,
                  .enabled = module.kind == ModuleKind::Core || enabled_.contains(module.id),
                  .lifecycle = module.kind == ModuleKind::Core ? ModuleLifecycle::Active : lifecycleLocked(module.id),
                  .hasData = hasData_.contains(module.id) && hasData_.at(module.id),
                  .dataPurgedAt = purgedAt_.contains(module.id) ? purgedAt_.at(module.id) : 0,
                  .sizeBytes = catalog_.sizeBytes(module),
                  .installedBytes = presentBytesLocked(module),
                  .hardware = hardwareLocked(module),
                  .job = std::nullopt,
                  .components = componentsLocked(module)};
  if (const auto job = jobs_.find(module.id); job != jobs_.end() && job->second.state != JobState::Done)
    view.job = jobViewLocked(job->second);
  return view;
}

std::vector<ModuleView> ModuleEngine::list() const
{
  const std::scoped_lock lock(mutex_);
  std::vector<ModuleView> views;
  views.reserve(catalog_.modules.size());
  for (const auto& module : catalog_.modules)
    views.push_back(viewLocked(module));
  return views;
}

ModuleView ModuleEngine::view(const std::string& moduleId) const
{
  const std::scoped_lock lock(mutex_);
  return viewLocked(moduleOrThrow(moduleId));
}

const CatalogModule& ModuleEngine::moduleOrThrow(const std::string& moduleId) const
{
  const auto* module = catalog_.module(moduleId);
  if (module == nullptr)
    throw ResponseException(ModuleErrors::UnknownModule);
  return *module;
}

void ModuleEngine::requireSettledLocked() const
{
  if (!settled_)
    throw ResponseException(ModuleErrors::NotSettled);
}

JobView ModuleEngine::install(const ModuleCommand& command)
{
  Outbox outbox;
  JobView result;
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    const auto& module = moduleOrThrow(command.moduleId);
    if (module.kind == ModuleKind::ComingSoon)
      throw ResponseException(ModuleErrors::ComingSoon);
    if (module.kind == ModuleKind::Core || enabled_.contains(module.id))
      throw ResponseException(ModuleErrors::AlreadyEnabled);
    if (openJobLocked(module.id))
      throw ResponseException(ModuleErrors::JobRunning);
    const auto order = module_resolver::installOrder(catalog_, module.id);
    for (const auto& id : order)
      if (const auto* entry = catalog_.module(id); entry != nullptr && entry->kind == ModuleKind::ComingSoon)
        throw ResponseException(ModuleErrors::ComingSoon);
    if (hardwareLocked(module).verdict == HardwareVerdict::Insufficient)
      throw ResponseException(ModuleErrors::HardwareInsufficient);

    const auto now = clock_();
    std::vector<ModuleJobSchema> created;
    transactLocked([&] {
    for (const auto& id : order) {
      const auto* entry = catalog_.module(id);
      if (entry == nullptr || enabled_.contains(id) || entry->kind == ModuleKind::Core || openJobLocked(id))
        continue;
      created.push_back(jobRepository_.create({.moduleId = id,
                                               .bytesTotal = catalog_.sizeBytes(*entry),
                                               .requestedBy = command.userId,
                                               .at = now}));
      static_cast<void>(auditRepository_.create({.moduleId = id,
                               .action = ModuleAuditAction::InstallRequested,
                               .userId = command.userId,
                               .detail = id == module.id ? std::string() : "requires:" + module.id,
                               .at = now}));
    }
    });
    for (const auto& job : created) {
      jobs_[job.moduleId] = job;
      if (throttle_.admit({.jobId = job.id, .state = job.state, .progress = 0, .nowMs = now}))
        outbox.modules.push_back(viewLocked(*catalog_.module(job.moduleId)));
    }
    result = jobViewLocked(jobs_.at(module.id));
    LOG_INFO << "Modules: user " << command.userId << " queued " << module.id << " (" << created.size() << " jobs)";
  }
  flush(outbox);
  wake();
  return result;
}

JobView ModuleEngine::pause(const ModuleCommand& command)
{
  Outbox outbox;
  JobView result;
  {
    const std::scoped_lock lock(mutex_);
    static_cast<void>(moduleOrThrow(command.moduleId));
    const auto job = openJobLocked(command.moduleId);
    if (!job)
      throw ResponseException(ModuleErrors::NoJob);
    if (job->state == JobState::Activating || job->state == JobState::HealthCheck)
      throw ResponseException(ModuleErrors::JobBusy);
    if (job->state != JobState::Paused) {
      transactLocked([&] {
      changeJobLocked({.jobId = job->id,
                       .moduleId = job->moduleId,
                       .update = {.at = clock_(),
                                  .state = JobState::Paused,
                                  .reason = std::nullopt,
                                  .owner = std::nullopt,
                                  .bytesDone = std::nullopt,
                                  .bytesTotal = std::nullopt}},
                      outbox);
      auditLocked(command, ModuleAuditAction::Paused, {});
      });
      if (job->state != JobState::Queued)
        pendingStops_.insert(command.moduleId);
    }
    result = jobViewLocked(jobs_.at(command.moduleId));
  }
  flush(outbox);
  wake();
  return result;
}

JobView ModuleEngine::resume(const ModuleCommand& command)
{
  Outbox outbox;
  JobView result;
  {
    const std::scoped_lock lock(mutex_);
    static_cast<void>(moduleOrThrow(command.moduleId));
    const auto job = openJobLocked(command.moduleId);
    if (!job)
      throw ResponseException(ModuleErrors::NoJob);
    if (job->state == JobState::Paused) {
      transactLocked([&] {
      changeJobLocked({.jobId = job->id,
                       .moduleId = job->moduleId,
                       .update = {.at = clock_(),
                                  .state = JobState::Queued,
                                  .reason = std::string(),
                                  .owner = std::nullopt,
                                  .bytesDone = std::nullopt,
                                  .bytesTotal = std::nullopt}},
                      outbox);
      auditLocked(command, ModuleAuditAction::Resumed, {});
      });
    }
    result = jobViewLocked(jobs_.at(command.moduleId));
  }
  flush(outbox);
  wake();
  return result;
}

JobView ModuleEngine::cancel(const ModuleCommand& command)
{
  Outbox outbox;
  JobView result;
  {
    const std::scoped_lock lock(mutex_);
    static_cast<void>(moduleOrThrow(command.moduleId));
    const auto job = openJobLocked(command.moduleId);
    if (!job)
      throw ResponseException(ModuleErrors::NoJob);
    if (job->state == JobState::Activating || job->state == JobState::HealthCheck)
      throw ResponseException(ModuleErrors::JobBusy);
    transactLocked([&] {
    changeJobLocked({.jobId = job->id,
                       .moduleId = job->moduleId,
                     .update = {.at = clock_(),
                                .state = JobState::Cancelled,
                                .reason = std::nullopt,
                                .owner = std::nullopt,
                                .bytesDone = std::nullopt,
                                .bytesTotal = std::nullopt}},
                    outbox);
    auditLocked(command, ModuleAuditAction::Cancelled, {});
    });
    pendingStops_.insert(command.moduleId);
    result = jobViewLocked(jobs_.at(command.moduleId));
  }
  flush(outbox);
  wake();
  return result;
}

ModuleView ModuleEngine::disable(const ModuleCommand& command)
{
  Outbox outbox;
  ModuleView result;
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    const auto& module = moduleOrThrow(command.moduleId);
    if (module.kind == ModuleKind::Core)
      throw ResponseException(ModuleErrors::CoreModule);
    if (openJobLocked(module.id))
      throw ResponseException(ModuleErrors::JobRunning);
    if (enabled_.contains(module.id)) {
      if (!module_resolver::enabledDependents(catalog_, module.id, enabled_).empty())
        throw ResponseException(ModuleErrors::RequiredBy);
      transactLocked([&] {
      setLifecycleLocked(command, ModuleLifecycle::Disabled, ModuleAuditAction::Disabled);
      });
      outbox.enabled = true;
      outbox.modules.push_back(viewLocked(module));
      LOG_INFO << "Modules: user " << command.userId << " disabled " << module.id;
    }
    result = viewLocked(module);
  }
  flush(outbox);
  return result;
}

std::vector<OwnerDataView> ModuleEngine::moduleData(const std::string& moduleId) const
{
  std::vector<std::string> owners;
  {
    const std::scoped_lock lock(mutex_);
    owners = moduleOrThrow(moduleId).dataOwners;
  }
  std::vector<OwnerDataView> views(owners.size());
  {
    std::vector<std::jthread> workers;
    workers.reserve(owners.size());
    for (const auto index : std::views::iota(std::size_t{0}, owners.size()))
      workers.emplace_back([this, &owners, &views, &moduleId, index] {
        auto reply = owners_.dataSummary(owners[index], moduleId);
        views[index] = {.owner = owners[index],
                        .reach = reply.reach,
                        .summary = reply.value.value_or(ModuleDataSummary{})};
      });
  }
  return views;
}

void ModuleEngine::refreshData(const std::vector<std::string>& moduleIds)
{
  std::map<std::string, bool> found;
  for (const auto& id : moduleIds) {
    bool any = false;
    for (const auto& view : moduleData(id))
      any = any || (view.reach == OwnerReach::Answered && !view.summary.empty());
    found[id] = any;
  }
  const std::scoped_lock lock(mutex_);
  for (const auto& [id, any] : found)
    hasData_[id] = any;
}

JobView ModuleEngine::uninstall(const UninstallCommand& command)
{
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    const auto& module = moduleOrThrow(command.moduleId);
    if (module.kind == ModuleKind::Core)
      throw ResponseException(ModuleErrors::CoreModule);
    if (openJobLocked(module.id))
      throw ResponseException(ModuleErrors::JobRunning);
    const auto dependents = module_resolver::enabledDependents(catalog_, module.id, enabled_);
    if (!dependents.empty())
      throw ResponseException(ModuleErrors::RequiredBy.withMessage("Required by " + dependents.front()));
  }
  if (!command.keepData) {
    const auto verdict =
        owners_.verifyPin(job_reason::kPinOwner, {.userId = command.userId, .pin = command.pin.value_or("")});
    if (verdict.reach == OwnerReach::Unreachable)
      throw ResponseException(ModuleErrors::PinUnverifiable);
    if (const auto& answer = verdict.value) {
      if (*answer == PinVerdict::Required)
        throw ResponseException(ModuleErrors::PinRequired);
      if (*answer == PinVerdict::Invalid)
        throw ResponseException(ModuleErrors::PinInvalid);
      if (*answer == PinVerdict::Locked)
        throw ResponseException(ModuleErrors::PinLocked);
    }
  }
  Outbox outbox;
  JobView result;
  {
    const std::scoped_lock lock(mutex_);
    requireSettledLocked();
    const auto& module = moduleOrThrow(command.moduleId);
    if (openJobLocked(module.id))
      throw ResponseException(ModuleErrors::JobRunning);
    if (!module_resolver::enabledDependents(catalog_, module.id, enabled_).empty())
      throw ResponseException(ModuleErrors::RequiredBy);
    const auto now = clock_();
    const ModuleCommand audit{.moduleId = module.id, .userId = command.userId};
    ModuleJobSchema created;
    const bool wasActive = enabled_.contains(module.id);
    transactLocked([&] {
      if (wasActive)
        setLifecycleLocked(audit, ModuleLifecycle::Disabled, ModuleAuditAction::Disabled);
      created = jobRepository_.create({.moduleId = module.id,
                                       .kind = command.keepData ? JobKind::Uninstall : JobKind::Purge,
                                       .bytesTotal = 0,
                                       .requestedBy = command.userId,
                                       .at = now});
      auditLocked(audit, ModuleAuditAction::UninstallRequested, command.keepData ? "keep_data" : "purge");
    });
    jobs_[module.id] = created;
    if (throttle_.admit({.jobId = created.id, .state = created.state, .progress = 0, .nowMs = now}))
      outbox.modules.push_back(viewLocked(module));
    outbox.enabled = wasActive;
    result = jobViewLocked(created);
    LOG_INFO << "Modules: user " << command.userId << " asked to uninstall " << module.id
             << (command.keepData ? " keeping its data" : " and purge its data");
  }
  flush(outbox);
  wake();
  return result;
}

drogon::Task<std::vector<ModuleView>> ModuleEngine::listAsync() const
{
  co_return co_await BlockingTask<std::vector<ModuleView>>([this] { return list(); });
}

drogon::Task<JobView> ModuleEngine::installAsync(ModuleCommand command)
{
  co_return co_await BlockingTask<JobView>([this, command = std::move(command)] { return install(command); });
}

drogon::Task<JobView> ModuleEngine::pauseAsync(ModuleCommand command)
{
  co_return co_await BlockingTask<JobView>([this, command = std::move(command)] { return pause(command); });
}

drogon::Task<JobView> ModuleEngine::resumeAsync(ModuleCommand command)
{
  co_return co_await BlockingTask<JobView>([this, command = std::move(command)] { return resume(command); });
}

drogon::Task<JobView> ModuleEngine::cancelAsync(ModuleCommand command)
{
  co_return co_await BlockingTask<JobView>([this, command = std::move(command)] { return cancel(command); });
}

drogon::Task<ModuleView> ModuleEngine::disableAsync(ModuleCommand command)
{
  co_return co_await BlockingTask<ModuleView>([this, command = std::move(command)] { return disable(command); });
}

drogon::Task<JobView> ModuleEngine::uninstallAsync(UninstallCommand command)
{
  co_return co_await BlockingTask<JobView>([this, command = std::move(command)] { return uninstall(command); });
}

drogon::Task<std::vector<OwnerDataView>> ModuleEngine::moduleDataAsync(std::string moduleId) const
{
  co_return co_await BlockingTask<std::vector<OwnerDataView>>(
      [this, moduleId = std::move(moduleId)] { return moduleData(moduleId); });
}

void ModuleEngine::wake()
{
  {
    const std::scoped_lock lock(waitMutex_);
    woken_ = true;
  }
  wakeUp_.notify_all();
}

void ModuleEngine::start()
{
  if (running_.exchange(true))
    return;
  worker_ = std::jthread([this](const std::stop_token& stop) { loop(stop); });
}

void ModuleEngine::loop(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    try {
      tick();
    }
    catch (const std::exception& error) {
      LOG_ERROR << "Modules: engine step failed: " << error.what();
    }
    std::unique_lock lock(waitMutex_);
    wakeUp_.wait_for(lock, stop, timing_.pollInterval, [this] { return woken_; });
    woken_ = false;
  }
  running_ = false;
}

void ModuleEngine::requestStop()
{
  if (worker_.joinable()) {
    worker_.request_stop();
    wakeUp_.notify_all();
  }
}

bool ModuleEngine::drained() const
{
  return !running_.load();
}
