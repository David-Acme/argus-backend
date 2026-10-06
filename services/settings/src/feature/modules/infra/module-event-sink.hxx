#pragma once

#include <feature/modules/schemas/module-view.hxx>
#include <settings/component-vocabulary.hxx>

#include <atomic>
#include <cstdint>
#include <string>

class NatsBus;

class ModuleEventSink
{
public:
  ModuleEventSink() = default;
  virtual ~ModuleEventSink() = default;
  ModuleEventSink(const ModuleEventSink&) = delete;
  ModuleEventSink& operator=(const ModuleEventSink&) = delete;
  ModuleEventSink(ModuleEventSink&&) = delete;
  ModuleEventSink& operator=(ModuleEventSink&&) = delete;

  virtual bool moduleChanged(const ModuleView& view, const ModuleStatesReply& set) = 0;
  virtual bool enabledChanged(const ModuleStatesReply& set) = 0;
};

namespace module_event
{
inline constexpr std::int64_t kMaxAgeNs = 24LL * 3600 * 1000000000LL;
inline constexpr std::int64_t kDuplicatesNs = 120LL * 1000000000LL;

[[nodiscard]] std::string modulePayload(const ModuleView& view, const ModuleStatesReply& set, std::int64_t atMs);
[[nodiscard]] std::string enabledPayload(const ModuleStatesReply& set, std::int64_t atMs);
}

class NatsModuleEventSink final : public ModuleEventSink
{
public:
  explicit NatsModuleEventSink(NatsBus& bus);

  bool moduleChanged(const ModuleView& view, const ModuleStatesReply& set) override;
  bool enabledChanged(const ModuleStatesReply& set) override;

  bool ensureStream();

private:
  bool publish(std::string payload);

  NatsBus& bus_;
  std::int64_t bootMs_{0};
  std::atomic<std::int64_t> sequence_{0};
};
