#pragma once

#include <doctest/doctest.h>

#include <mcp/client.hxx>
#include <mcp/json-rpc.hxx>

#include <optional>
#include <string>
#include <utility>

namespace test_support
{

template <class T>
T must(std::optional<T> value)
{
  REQUIRE(value.has_value());
  return std::move(value).value_or(T{});
}

template <class T>
T valueOf(const argus::mcp::Response<T>& response)
{
  REQUIRE(response.ok());
  return response.valueOrDefault();
}

inline Json::Value resultOf(const argus::mcp::RpcResponse& response)
{
  REQUIRE(response.result.has_value());
  return response.result.value_or(Json::Value());
}

inline argus::mcp::RpcError errorOf(const argus::mcp::RpcResponse& response)
{
  REQUIRE(response.error.has_value());
  return response.error.value_or(argus::mcp::RpcError{});
}

inline argus::mcp::RpcResponse answer(const std::string& frame)
{
  return must(argus::mcp::parseResponse(frame));
}

inline Json::Value json(const std::string& text)
{
  return must(argus::mcp::parseJson(text));
}

}
