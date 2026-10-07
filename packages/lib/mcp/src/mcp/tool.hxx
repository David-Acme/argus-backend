#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace argus::mcp
{

inline constexpr std::string_view kProtocolVersion = "2026-07-28";
inline constexpr std::string_view kVersionKey = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view kClientInfoKey = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view kClientCapabilitiesKey = "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view kServerInfoKey = "io.modelcontextprotocol/serverInfo";
inline constexpr std::string_view kModuleKey = "argus/module";
inline constexpr std::string_view kCapabilityKey = "argus/capability";
inline constexpr std::string_view kContextKey = "argus/context";
inline constexpr std::string_view kAppActionKey = "argus/appAction";

struct ToolAnnotations
{
  bool readOnly{false};
  bool destructive{false};
  bool idempotent{false};
  bool openWorld{false};
};

struct ToolSpec
{
  std::string name;
  std::string title;
  std::string description;
  Json::Value inputSchema{Json::objectValue};
  ToolAnnotations annotations;
  std::string module{};
  std::string capability;
};

struct CallerContext
{
  int64_t userId{0};
  std::string role;
  std::string lang;
  std::string sessionId;
  std::string utterance;
  bool decided{false};
};

struct ToolInvocation
{
  std::string name;
  Json::Value arguments{Json::objectValue};
  CallerContext caller;
};

struct AppAction
{
  std::string name;
  Json::Value arguments{Json::objectValue};
};

struct ToolOutcome
{
  bool isError{false};
  std::string text;
  Json::Value structured;
  std::optional<AppAction> appAction;
};

struct ToolFailure
{
  std::string text;
  std::string code;
};

[[nodiscard]] ToolOutcome toolFailure(const ToolFailure& failure);

[[nodiscard]] bool validToolName(std::string_view name);

[[nodiscard]] Json::Value toJson(const ToolSpec& spec);

[[nodiscard]] std::optional<ToolSpec> toolSpecFrom(const Json::Value& json);

[[nodiscard]] Json::Value toJson(const CallerContext& caller);

[[nodiscard]] CallerContext callerContextFrom(const Json::Value& json);

[[nodiscard]] Json::Value toJson(const ToolOutcome& outcome);

[[nodiscard]] std::optional<ToolOutcome> toolOutcomeFrom(const Json::Value& result);

[[nodiscard]] Json::Value requestMeta(const Json::Value& clientInfo);

}
