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
  // The user message the call answers; the loop sets it so a handler can
  // fall back to the triggering sentence when the model's arguments are
  // incomplete (see the f8-b4 record).
  std::string utterance;
  // An upstream classifier already decided this turn's intent, so a handler
  // must not re-derive it from an explicit trigger the sentence may not
  // carry: 39.5% of real memory_save utterances have no rule clause.
  bool decided = false;
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
