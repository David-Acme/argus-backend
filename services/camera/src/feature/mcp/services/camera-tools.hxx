#pragma once

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
};

using CameraCatalog = std::function<drogon::Task<std::vector<CameraChoice>>()>;

struct CameraToolsInput
{
  CameraCatalog catalog;
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
