#pragma once

#include <auth/user-role.hxx>
#include <mcp/tool.hxx>

#include <cstdint>
#include <functional>
#include <optional>
#include <json/value.h>
#include <memory>
#include <string>
#include <vector>

namespace tools
{

struct ToolContext
{
  int64_t userId = 0;
  UserRole role = UserRole::Unknown;
  std::string lang = "es";
  std::string sessionId;
  std::string channel = "tool_result";
  std::string utterance;
  bool decided = false;
  int64_t turn = 0;
  std::function<void(const std::string& name, const Json::Value& arguments)> emitAction = {};
  std::optional<int64_t> heardAt = std::nullopt;
  std::optional<int64_t> now = std::nullopt;
};

struct ToolCall
{
  std::string name;
  Json::Value arguments;
  ToolContext context;
};

struct ToolResult
{
  std::string tool;
  bool ok = false;
  std::string output;
  Json::Value data = Json::Value(Json::objectValue);
  std::string code;
};

struct ToolDescriptor
{
  argus::mcp::ToolSpec spec;
  std::function<ToolResult(const ToolCall&)> handler;
};

using ToolHandle = std::shared_ptr<const ToolDescriptor>;

}
