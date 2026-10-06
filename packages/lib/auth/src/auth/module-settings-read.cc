#include "module-feed.hxx"

#include <config/config-service.hxx>
#include <settings/settings-client.hxx>

#include <trantor/utils/Logger.h>

#include <exception>
#include <memory>

namespace
{
constexpr const char* kTargetKey = "modules.target";
constexpr const char* kCredentialKey = "modules.credential";
constexpr std::chrono::milliseconds kBootReadTimeout{3000};

ModuleIntroText introOf(const ModuleIntroLine& line)
{
  return {.what = line.what, .examples = line.examples};
}

std::optional<ModuleFeed::Snapshot> snapshotOf(const ModuleStatesReply& reply)
{
  if (!reply.settled)
    return std::nullopt;
  ModuleFeed::Snapshot snapshot{.flags = {}, .version = reply.version, .epoch = reply.epoch};
  snapshot.flags.reserve(reply.modules.size());
  for (const auto& module : reply.modules)
    snapshot.flags.push_back({.id = module.id,
                              .enabled = module.enabled,
                              .lifecycle = module.lifecycle,
                              .dataPurgedAt = module.dataPurgedAt,
                              .roles = module.roles,
                              .name = {.es = module.name.es, .en = module.name.en},
                              .summary = {.es = module.summary.es, .en = module.summary.en},
                              .intro = {.es = introOf(module.intro.es), .en = introOf(module.intro.en)},
                              .kind = module.kind});
  return snapshot;
}
}

namespace module_gate
{
ModuleFeed::BootRead settingsBootRead()
{
  const std::string target = ConfigService::getString(kTargetKey);
  if (target.empty()) {
    LOG_INFO << "Modules: no [modules] target; the enabled set arrives from the feed alone";
    return {};
  }
  std::shared_ptr<const ModulesClient> client;
  try {
    client = std::make_shared<const ModulesClient>(
        SettingsClientConfig{.target = target,
                             .credential = ConfigService::getString(kCredentialKey),
                             .timeout = kBootReadTimeout});
  }
  catch (const std::exception& error) {
    LOG_WARN << "Modules: the [modules] target is unusable (" << error.what() << ")";
    return {};
  }
  return [client]() -> std::optional<ModuleFeed::Snapshot> {
    try {
      return snapshotOf(client->moduleStates());
    }
    catch (const std::exception& error) {
      LOG_WARN << "Modules: settings did not answer the enabled set (" << error.what() << ")";
      return std::nullopt;
    }
  };
}
}
