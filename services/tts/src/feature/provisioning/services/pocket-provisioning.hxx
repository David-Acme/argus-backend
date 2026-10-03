#pragma once

#include <config/settings-registry.hxx>
#include <feature/provisioning/infra/provision-process.hxx>
#include <feature/provisioning/services/pocket-choice-states.hxx>
#include <feature/provisioning/services/pocket-installer.hxx>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct PocketProvisioningConfig
{
  ProvisioningPaths paths;
  std::filesystem::path configFile;
};

class PocketProvisioning
{
public:
  explicit PocketProvisioning(PocketProvisioningConfig config);

  [[nodiscard]] std::vector<ChoiceState> choiceStates(const SettingSpec& spec) const;
  void installFor(const std::vector<std::string>& changedKeys);
  [[nodiscard]] const ProvisioningHost& host() const;
  void stop();

  [[nodiscard]] static bool nonCommercialAllowed();

private:
  [[nodiscard]] PocketProvisioningView view() const;

  PocketProvisioningConfig config_;
  ProvisioningHost host_;
  std::unique_ptr<PocketInstaller> installer_;
};
