#pragma once

#include <config/settings-config.hxx>
#include <feature/settings/services/settings-gateway-service.hxx>
#include <feature/settings/services/settings-profile.hxx>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct FirstRunInput
{
  const SettingsGatewayService& gateway;
  std::optional<ProfileCatalog> catalog;
  HardwareFacts hardware;
  FirstRunConfig config;
};

struct FirstRunPass
{
  std::vector<std::string> settled;
  std::vector<std::string> waiting;
};

class FirstRunService
{
public:
  explicit FirstRunService(FirstRunInput input);
  ~FirstRunService();
  FirstRunService(const FirstRunService&) = delete;
  FirstRunService& operator=(const FirstRunService&) = delete;

  void start();
  FirstRunPass runOnce();

  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  void loop(const std::stop_token& stop);

  const SettingsGatewayService& gateway_;
  std::optional<ProfileCatalog> catalog_;
  HardwareFacts hardware_;
  FirstRunConfig config_;
  std::vector<std::string> settled_;
  std::mutex waitMutex_;
  std::condition_variable_any wake_;
  std::atomic<bool> running_{false};
  std::jthread worker_;
};
