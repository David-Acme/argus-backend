#pragma once

#include <functional>
#include <json/value.h>
#include <shared/access/role-access.hxx>
#include <string>
#include <vector>

// Tool runtime contracts (COGNITIVE_MEMORY_PLAN.md §6), namespaced so the legacy memory ToolCall stays unambiguous.
// argus-memory declares descriptors in these shapes and argus-llm executes them; nothing here crosses the chat wire
// (it carries toolsEnabled, and the declarations are built server-side from the registry), so the header sits in this
// package only because a tier argus-memory and argus-llm may both reach is where a shared vocabulary can live — the
// registry, validator and executor that ran over it moved to argus-llm with Phase 1 step 9.
namespace tools
{

struct ToolContext
{
  int64_t userId = 0;
  std::string lang = "es";
  std::string sessionId;
  std::string channel = "tool_result";
  // The user message the call answers; handlers fall back to it.
  std::string utterance;
  // An upstream classifier already decided this turn's intent.
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
