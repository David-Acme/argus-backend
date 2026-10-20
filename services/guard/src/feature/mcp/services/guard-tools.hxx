#pragma once

#include <mcp/server.hxx>
#include <shared/vocabulary/guard-status.hxx>
#include <shared/vocabulary/maintenance-voice.hxx>
#include <shared/vocabulary/masked-camera.hxx>
#include <shared/vocabulary/whereabouts-voice.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>
#include <vector>

struct EnvironmentChoice
{
  int64_t id{0};
  std::string name;
};

using EnvironmentCatalog = std::function<drogon::Task<std::vector<EnvironmentChoice>>()>;

struct GuardToolsInput
{
  EnvironmentCatalog catalog;
  MaskedCameraDirectory maskedCameras{};
  std::function<trantor::EventLoop*()> loop;
  MaintenanceVoicePorts maintenance{};
  WhereaboutsVoice whereabouts{};
  GuardStatusDirectory status{};
};

struct GuardStatusLineInput
{
  const GuardEnvironmentStatus& environment;
  bool english{false};
  int64_t now{0};
};

[[nodiscard]] std::string guardStatusLine(const GuardStatusLineInput& input);

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> guardToolServer(const GuardToolsInput& input);
