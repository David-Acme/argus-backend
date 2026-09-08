#pragma once

#include <functional>
#include <json/value.h>
#include <shared/access/role-access.hxx>
#include <string>
#include <vector>

// Tool runtime contracts (COGNITIVE_MEMORY_PLAN.md §6), namespaced so the legacy memory ToolCall stays unambiguous.
namespace tools
{

struct ToolContext
{
  int64_t userId = 0;
  std::string lang = "es";
  std::string sessionId;
  std::string channel = "tool_result";
};

struct ToolCall
{
  std::string name;
  Json::Value arguments; // JSON object; a missing key means "absent"
  ToolContext context;
};

struct ToolResult
{
  std::string tool;
  bool ok = false;
  std::string output;
  Json::Value data = Json::Value(Json::objectValue);
};

struct ToolArgumentSpec
{
  std::string name;
  std::string type; // string | number | boolean | enum
  bool required = false;
  std::vector<std::string> enumValues;
  std::string description;
};

struct ToolDescriptor
{
  std::string name;
  std::string description;
  std::vector<ToolArgumentSpec> arguments;
  TableName accessTable;
  RolePermission accessPermission;
  std::function<ToolResult(const ToolCall&)> handler;
};

} // namespace tools
