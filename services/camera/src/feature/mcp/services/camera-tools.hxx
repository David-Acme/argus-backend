#pragma once

#include <camera/camera-privacy-state.hxx>
#include <mcp/confirmation.hxx>
#include <mcp/server.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>
#include <vector>

struct CameraChoice
{
  int64_t id{0};
  std::string name;
  CameraPrivacyState privacyState{CameraPrivacyState::Unknown};
};

using CameraCatalog = std::function<drogon::Task<std::vector<CameraChoice>>()>;

struct HouseholdPosture
{
  bool protects{false};
  bool armed{false};
};

using PostureDirectory = std::function<HouseholdPosture()>;

struct CameraToolsInput
{
  CameraCatalog catalog;
  PostureDirectory posture{};
  std::function<trantor::EventLoop*()> loop;
};

enum class CameraMatchKind : uint8_t
{
  Exact,
  Missing,
  Ambiguous
};

struct CameraMatch
{
  CameraMatchKind kind{CameraMatchKind::Missing};
  CameraChoice camera;
  std::vector<CameraChoice> candidates;
};

struct CameraQuery
{
  const std::vector<CameraChoice>& cameras;
  std::string text;
};

[[nodiscard]] CameraMatch matchCamera(const CameraQuery& query);

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> cameraToolServer(const CameraToolsInput& input);
