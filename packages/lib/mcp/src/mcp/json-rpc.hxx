#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace argus::mcp
{

enum class ErrorCode : std::int16_t
{
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,
  MissingClientCapability = -32021,
  UnsupportedProtocolVersion = -32022
};

[[nodiscard]] constexpr int codeOf(ErrorCode code)
{
  return static_cast<int>(code);
}

struct RpcError
{
  int code{codeOf(ErrorCode::InternalError)};
  std::string message;
  Json::Value data;
};

struct RpcRequest
{
  bool hasId{false};
  Json::Value id;
  std::string method;
  Json::Value params{Json::objectValue};

  [[nodiscard]] bool notification() const { return !hasId; }
};

struct RpcRejection
{
  Json::Value id;
  RpcError error;
};

using ParsedRequest = std::variant<RpcRequest, RpcRejection>;

struct RpcResponse
{
  Json::Value id;
  std::optional<Json::Value> result;
  std::optional<RpcError> error;
};

[[nodiscard]] std::optional<Json::Value> parseJson(std::string_view text);

[[nodiscard]] std::string serialize(const Json::Value& value);

[[nodiscard]] ParsedRequest parseRequest(std::string_view frame);

[[nodiscard]] std::optional<RpcResponse> parseResponse(std::string_view frame);

[[nodiscard]] std::string requestFrame(const RpcRequest& request);

struct RpcResult
{
  Json::Value id;
  Json::Value result;
};

[[nodiscard]] std::string resultFrame(const RpcResult& result);

[[nodiscard]] std::string errorFrame(const Json::Value& id, const RpcError& error);

}
