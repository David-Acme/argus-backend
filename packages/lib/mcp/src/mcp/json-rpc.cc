#include "json-rpc.hxx"

#include <json/reader.h>
#include <json/writer.h>

#include <memory>
#include <utility>

namespace argus::mcp
{

namespace
{
constexpr int kStackLimit = 64;
constexpr std::string_view kVersion = "2.0";

bool validId(const Json::Value& id)
{
  return id.isString() || id.isIntegral();
}

RpcRejection reject(ErrorCode code, std::string message)
{
  return {.id = Json::Value(), .error = {.code = codeOf(code), .message = std::move(message), .data = Json::Value()}};
}

Json::Value envelope(const Json::Value& id)
{
  Json::Value frame(Json::objectValue);
  frame["jsonrpc"] = std::string(kVersion);
  frame["id"] = id;
  return frame;
}
}

std::optional<Json::Value> parseJson(std::string_view text)
{
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder["stackLimit"] = kStackLimit;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  std::string errors;
  try {
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &errors))
      return std::nullopt;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
  return value;
}

std::string serialize(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  return Json::writeString(builder, value);
}

ParsedRequest parseRequest(std::string_view frame)
{
  const auto parsed = parseJson(frame);
  if (!parsed)
    return reject(ErrorCode::ParseError, "Parse error");
  const Json::Value& json = *parsed;
  if (!json.isObject())
    return reject(ErrorCode::InvalidRequest, "A request must be a JSON object");
  RpcRejection rejection;
  rejection.id = json.isMember("id") && validId(json["id"]) ? json["id"] : Json::Value();
  if (!json["jsonrpc"].isString() || json["jsonrpc"].asString() != kVersion) {
    rejection.error = {.code = codeOf(ErrorCode::InvalidRequest),
                       .message = "jsonrpc must be \"2.0\"",
                       .data = Json::Value()};
    return rejection;
  }
  if (!json["method"].isString() || json["method"].asString().empty()) {
    rejection.error = {.code = codeOf(ErrorCode::InvalidRequest),
                       .message = "method must be a non-empty string",
                       .data = Json::Value()};
    return rejection;
  }
  if (json.isMember("id") && !validId(json["id"])) {
    rejection.error = {.code = codeOf(ErrorCode::InvalidRequest),
                       .message = "id must be a string or an integer",
                       .data = Json::Value()};
    return rejection;
  }
  if (json.isMember("params") && !json["params"].isObject()) {
    rejection.error = {.code = codeOf(ErrorCode::InvalidParams),
                       .message = "params must be an object",
                       .data = Json::Value()};
    return rejection;
  }
  RpcRequest request;
  request.hasId = json.isMember("id");
  request.id = request.hasId ? json["id"] : Json::Value();
  request.method = json["method"].asString();
  request.params = json.isMember("params") ? json["params"] : Json::Value(Json::objectValue);
  return request;
}

std::optional<RpcResponse> parseResponse(std::string_view frame)
{
  const auto parsed = parseJson(frame);
  if (!parsed || !parsed->isObject() || !(*parsed)["jsonrpc"].isString() ||
      (*parsed)["jsonrpc"].asString() != kVersion)
    return std::nullopt;
  const Json::Value& json = *parsed;
  RpcResponse response;
  response.id = json.isMember("id") ? json["id"] : Json::Value();
  if (json.isMember("error")) {
    const Json::Value& error = json["error"];
    if (!error.isObject() || !error["code"].isIntegral() || !error["message"].isString())
      return std::nullopt;
    response.error = RpcError{.code = error["code"].asInt(),
                              .message = error["message"].asString(),
                              .data = error.isMember("data") ? error["data"] : Json::Value()};
    return response;
  }
  if (!json.isMember("result") || !json["result"].isObject())
    return std::nullopt;
  response.result = json["result"];
  return response;
}

std::string requestFrame(const RpcRequest& request)
{
  Json::Value frame(Json::objectValue);
  frame["jsonrpc"] = std::string(kVersion);
  if (request.hasId)
    frame["id"] = request.id;
  frame["method"] = request.method;
  frame["params"] = request.params;
  return serialize(frame);
}

std::string resultFrame(const RpcResult& result)
{
  Json::Value frame = envelope(result.id);
  frame["result"] = result.result;
  return serialize(frame);
}

std::string errorFrame(const Json::Value& id, const RpcError& error)
{
  Json::Value frame = envelope(id);
  Json::Value body(Json::objectValue);
  body["code"] = error.code;
  body["message"] = error.message;
  if (!error.data.isNull())
    body["data"] = error.data;
  frame["error"] = std::move(body);
  return serialize(frame);
}

}
