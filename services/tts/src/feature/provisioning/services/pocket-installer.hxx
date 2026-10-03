#pragma once

#include <feature/provisioning/services/pocket-choice-states.hxx>

#include <sys/types.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PocketInstallerConfig
{
  std::filesystem::path script;
  std::filesystem::path configFile;
  std::function<bool()> nonCommercialAllowed;
};

class PocketInstaller
{
public:
  explicit PocketInstaller(PocketInstallerConfig config);
  ~PocketInstaller();

  PocketInstaller(const PocketInstaller&) = delete;
  PocketInstaller& operator=(const PocketInstaller&) = delete;
  PocketInstaller(PocketInstaller&&) = delete;
  PocketInstaller& operator=(PocketInstaller&&) = delete;

  void request(const std::vector<PocketComponent>& components);
  [[nodiscard]] std::optional<PocketInstallJob> job(const PocketComponent& component) const;
  void stop();

private:
  void work(const std::stop_token& stop);
  void install(const PocketComponent& component);

  PocketInstallerConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<PocketComponent> queue_;
  std::unordered_map<std::string, PocketInstallJob> jobs_;
  pid_t child_{0};
  bool stopping_{false};
  std::jthread worker_;
};
