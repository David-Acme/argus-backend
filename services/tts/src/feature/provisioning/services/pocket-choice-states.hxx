#pragma once

#include <config/settings-registry.hxx>
#include <feature/provisioning/infra/pocket-catalog.hxx>
#include <feature/provisioning/infra/provision-process.hxx>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class PocketComponentKind : std::uint8_t
{
  Variant,
  Voice
};

struct PocketComponent
{
  PocketComponentKind kind{PocketComponentKind::Variant};
  std::string variant;
  std::string voice;

  [[nodiscard]] std::string id() const;
  [[nodiscard]] std::vector<std::string> arguments() const;
  [[nodiscard]] bool operator==(const PocketComponent& other) const = default;
};

enum class PocketInstallJob : std::uint8_t
{
  Running,
  Failed
};

struct PocketProvisioningView
{
  PocketCatalog catalog;
  ProvisioningHost host;
  bool nonCommercialAllowed{false};
  std::function<bool(const PocketComponent&)> installed;
  std::function<std::optional<PocketInstallJob>(const PocketComponent&)> job;
};

[[nodiscard]] std::vector<ChoiceState> pocketChoiceStates(const SettingSpec& spec, const PocketProvisioningView& view);
[[nodiscard]] std::vector<PocketComponent> pocketComponentsToInstall(const std::vector<std::string>& changedKeys,
                                                                     const PocketProvisioningView& view);
[[nodiscard]] std::string pocketHostCommand(const PocketComponent& component, bool nonCommercial);
[[nodiscard]] bool pocketComponentPresent(const std::filesystem::path& pocketDir, const PocketComponent& component);
