#pragma once

#include <sync/user-action-event.hxx>

#include <atomic>
#include <cstdint>
#include <string>

class NatsBus;

struct ModuleActionRecord
{
  std::int64_t auditId{0};
  UserActionEvent event;
};

class ModuleActionSink
{
public:
  ModuleActionSink() = default;
  virtual ~ModuleActionSink() = default;
  ModuleActionSink(const ModuleActionSink&) = delete;
  ModuleActionSink& operator=(const ModuleActionSink&) = delete;
  ModuleActionSink(ModuleActionSink&&) = delete;
  ModuleActionSink& operator=(ModuleActionSink&&) = delete;

  virtual bool publish(const ModuleActionRecord& record) = 0;
};

namespace module_action
{
inline constexpr std::int64_t kMaxAgeNs = 7LL * 24 * 3600 * 1000000000LL;
inline constexpr std::int64_t kDuplicatesNs = 120LL * 1000000000LL;

[[nodiscard]] std::string msgId(std::int64_t auditId);
}

class NatsModuleActionSink final : public ModuleActionSink
{
public:
  explicit NatsModuleActionSink(NatsBus& bus);

  bool publish(const ModuleActionRecord& record) override;
  bool ensureStream();

private:
  NatsBus& bus_;
  std::atomic<bool> streamReady_{false};
};
