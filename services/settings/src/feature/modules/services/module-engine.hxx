#pragma once

#include <config/settings-config.hxx>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/modules/infra/component-owners.hxx>
#include <feature/modules/infra/module-event-sink.hxx>
#include <feature/modules/repositories/module-audit/module-audit-repository.hxx>
#include <feature/modules/repositories/module-job/module-job-repository.hxx>
#include <feature/modules/repositories/module-purge/module-purge-repository.hxx>
#include <feature/modules/repositories/module-state/module-state-repository.hxx>
#include <feature/modules/schemas/module-view.hxx>
#include <feature/modules/services/module-event-throttle.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct ModuleEngineInput
{
  ModuleCatalog catalog;
  drogon::orm::DbClientPtr db;
  const ComponentOwners& owners;
  ModuleEventSink* events{nullptr};
  std::function<HostResources()> host;
  std::function<std::int64_t()> clock;
  ModulesConfig timing;
};

struct ModuleCommand
{
  std::string moduleId;
  std::int64_t userId{0};
};

struct UninstallCommand
{
  std::string moduleId;
  std::int64_t userId{0};
  bool keepData{true};
  std::optional<std::string> pin;
};

struct OwnerDataView
{
  std::string owner;
  OwnerReach reach{OwnerReach::Unreachable};
  ModuleDataSummary summary;
};

namespace job_reason
{
inline constexpr const char* kHardwareInsufficient = "hardware_insufficient";
inline constexpr const char* kHostOnly = "host_only";
inline constexpr const char* kHealthCheckFailed = "health_check_failed";
inline constexpr const char* kDependencyFailed = "dependency_failed";
inline constexpr const char* kOwnerUnreachable = "owner_unreachable";
inline constexpr const char* kRemoveFailed = "remove_failed";
inline constexpr const char* kRemoveUnsupported = "remove_unsupported";
inline constexpr const char* kPurgeFailed = "purge_failed";
inline constexpr const char* kPurgeUnsupported = "purge_unsupported";
inline constexpr const char* kPinOwner = "guard";
}

class ModuleEngine
{
public:
  explicit ModuleEngine(ModuleEngineInput input);
  ~ModuleEngine();
  ModuleEngine(const ModuleEngine&) = delete;
  ModuleEngine& operator=(const ModuleEngine&) = delete;
  ModuleEngine(ModuleEngine&&) = delete;
  ModuleEngine& operator=(ModuleEngine&&) = delete;

  [[nodiscard]] std::vector<ModuleView> list() const;
  [[nodiscard]] ModuleView view(const std::string& moduleId) const;
  [[nodiscard]] ModuleStatesReply enabledSet() const;

  JobView install(const ModuleCommand& command);
  JobView pause(const ModuleCommand& command);
  JobView resume(const ModuleCommand& command);
  JobView cancel(const ModuleCommand& command);
  ModuleView disable(const ModuleCommand& command);
  JobView uninstall(const UninstallCommand& command);
  [[nodiscard]] std::vector<OwnerDataView> moduleData(const std::string& moduleId) const;

  [[nodiscard]] drogon::Task<std::vector<ModuleView>> listAsync() const;
  [[nodiscard]] drogon::Task<JobView> installAsync(ModuleCommand command);
  [[nodiscard]] drogon::Task<JobView> pauseAsync(ModuleCommand command);
  [[nodiscard]] drogon::Task<JobView> resumeAsync(ModuleCommand command);
  [[nodiscard]] drogon::Task<JobView> cancelAsync(ModuleCommand command);
  [[nodiscard]] drogon::Task<ModuleView> disableAsync(ModuleCommand command);
  [[nodiscard]] drogon::Task<JobView> uninstallAsync(UninstallCommand command);
  [[nodiscard]] drogon::Task<std::vector<OwnerDataView>> moduleDataAsync(std::string moduleId) const;

  void tick();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct Rate
  {
    std::int64_t bytes{0};
    std::int64_t atMs{0};
    double bytesPerSecond{0};
  };

  struct ComponentReading
  {
    ComponentStatus status;
    OwnerReach reach{OwnerReach::Unreachable};
    bool known{false};
  };

  struct Outbox
  {
    std::vector<ModuleView> modules;
    bool enabled{false};
  };

  struct JobChange
  {
    std::int64_t jobId{0};
    std::string moduleId;
    ModuleJobUpdateInput update;
  };

  struct Transition
  {
    std::int64_t jobId{0};
    JobState from{JobState::Queued};
    ModuleJobUpdateInput update;
  };

  struct OwnerCall
  {
    std::string owner;
    ComponentSpec spec;
  };

  struct StepPlan
  {
    std::optional<Transition> transition;
    std::vector<OwnerCall> installs;
    std::vector<OwnerCall> cancels;
    std::vector<OwnerCall> removes;
    std::vector<std::string> purges;
    bool activate{false};
    bool rollback{false};
    bool finishUninstall{false};
  };

  struct OwnerFailure
  {
    std::string owner;
    std::string reason;
  };

  void load();
  void reloadLocked();
  template <typename Body>
  void transactLocked(Body&& body);
  void loop(const std::stop_token& stop);
  void refresh(const std::set<std::string>& owners);
  [[nodiscard]] std::set<std::string> ownersOf(const CatalogModule& module) const;
  [[nodiscard]] std::set<std::string> allOwners() const;

  void seedLocked(Outbox& outbox);
  [[nodiscard]] bool seedReadyLocked() const;
  [[nodiscard]] std::optional<ModuleJobSchema> nextJobLocked(Outbox& outbox);
  [[nodiscard]] StepPlan planLocked(const ModuleJobSchema& job, const CatalogModule& module);
  void applyLocked(const StepPlan& plan, const CatalogModule& module, Outbox& outbox);
  void setLifecycleLocked(const ModuleCommand& command, ModuleLifecycle lifecycle, ModuleAuditAction action);
  [[nodiscard]] ModuleLifecycle lifecycleLocked(const std::string& moduleId) const;
  [[nodiscard]] StepPlan planInstallLocked(const ModuleJobSchema& job, const CatalogModule& module);
  [[nodiscard]] StepPlan planUninstallLocked(const ModuleJobSchema& job, const CatalogModule& module);
  [[nodiscard]] std::vector<OwnerCall> removalsLocked(const CatalogModule& module) const;
  [[nodiscard]] std::optional<OwnerFailure> runOwnerCalls(StepPlan& plan, const CatalogModule& module);
  void refreshData(const std::vector<std::string>& moduleIds);
  void changeJobLocked(const JobChange& change, Outbox& outbox);
  void auditLocked(const ModuleCommand& command, ModuleAuditAction action, const std::string& detail);
  void progressLocked(ModuleJobSchema& job, const CatalogModule& module, Outbox& outbox);

  [[nodiscard]] ModuleView viewLocked(const CatalogModule& module) const;
  [[nodiscard]] JobView jobViewLocked(const ModuleJobSchema& job) const;
  [[nodiscard]] std::vector<ComponentView> componentsLocked(const CatalogModule& module) const;
  [[nodiscard]] std::int64_t presentBytesLocked(const CatalogModule& module) const;
  [[nodiscard]] std::int64_t remainingBytesLocked(const CatalogModule& module) const;
  [[nodiscard]] HardwareAssessment hardwareLocked(const CatalogModule& module) const;
  [[nodiscard]] ModuleStatesReply enabledSetLocked() const;
  [[nodiscard]] std::optional<ModuleJobSchema> openJobLocked(const std::string& moduleId) const;
  [[nodiscard]] const CatalogModule& moduleOrThrow(const std::string& moduleId) const;
  void requireSettledLocked() const;

  void flush(const Outbox& outbox);
  void wake();

  ModuleCatalog catalog_;
  drogon::orm::DbClientPtr db_;
  const ComponentOwners& owners_;
  ModuleEventSink* events_;
  std::function<HostResources()> host_;
  std::function<std::int64_t()> clock_;
  ModulesConfig timing_;
  ModuleStateRepository stateRepository_;
  ModuleJobRepository jobRepository_;
  ModuleAuditRepository auditRepository_;
  ModulePurgeRepository purgeRepository_;

  mutable std::mutex mutex_;
  std::mutex tickMutex_;
  std::mutex publishMutex_;
  std::set<std::string> enabled_;
  std::map<std::string, ModuleLifecycle> lifecycles_;
  std::map<std::string, std::int64_t> purgedAt_;
  std::map<std::string, bool> hasData_;
  bool settled_{false};
  std::int64_t version_{0};
  std::int64_t publishedVersion_{-1};
  std::int64_t bootMs_{0};
  std::int64_t refreshedAt_{0};
  std::map<std::string, ModuleJobSchema> jobs_;
  std::map<std::string, ComponentReading> readings_;
  std::map<std::int64_t, Rate> rates_;
  std::set<std::string> pendingStops_;
  ModuleEventThrottle throttle_;

  std::mutex waitMutex_;
  std::condition_variable_any wakeUp_;
  bool woken_{false};
  std::atomic<bool> running_{false};
  std::jthread worker_;
};
