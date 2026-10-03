#pragma once

#include <sys/types.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ProvisioningPaths
{
  std::filesystem::path modelsDir;
  std::filesystem::path pocketDir;
  std::filesystem::path toolchainPython;
};

struct ProvisioningHost
{
  std::optional<std::filesystem::path> script;
  bool canDownload{false};
  bool canExport{false};
};

struct ProvisionCommand
{
  std::filesystem::path script;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::function<void(std::string_view)> onLine;
  std::function<void(pid_t)> onStart;
};

[[nodiscard]] std::optional<std::filesystem::path> provisionScriptFor(const ProvisioningPaths& paths);
[[nodiscard]] ProvisioningHost probeProvisioningHost(const ProvisioningPaths& paths);
[[nodiscard]] std::filesystem::path defaultToolchainPython();
[[nodiscard]] bool onPath(std::string_view program);
int runProvisioning(const ProvisionCommand& command);
