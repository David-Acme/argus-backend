#pragma once

#include <functional>
#include <json/value.h>
#include <string>
#include <sync/role-permission.hxx>
#include <sync/table-name.hxx>
#include <vector>

namespace tools
{

struct ToolContext
{
  int64_t userId = 0;
  std::string lang = "es";
  std::string sessionId;
  std::string channel = "tool_result";
  std::string utterance;
  bool decided = false;
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
};

struct ToolArgumentSpec
{
  std::string name;
  std::string type;
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

}
