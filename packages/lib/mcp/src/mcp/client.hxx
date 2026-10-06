#pragma once

#include <mcp/json-rpc.hxx>
#include <mcp/tool.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace argus::mcp
{

class Transport
{
public:
  Transport() = default;
  virtual ~Transport() = default;
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  [[nodiscard]] virtual std::optional<std::string> exchange(const std::string& frame) = 0;
};

struct Failure
{
  enum class Kind : std::uint8_t
  {
    None,
    Transport,
    Protocol,
    Malformed
  };

  Kind kind{Kind::None};
  RpcError error;
};

template <class T>
struct Response
{
  std::optional<T> value;
  Failure failure;

  [[nodiscard]] bool ok() const { return value.has_value(); }

  [[nodiscard]] T valueOrDefault() const { return value.value_or(T{}); }
};

struct ServerDescription
{
  std::vector<std::string> supportedVersions;
  std::string name;
  std::string version;
  std::string instructions;
  bool tools{false};
};

struct ToolList
{
  std::vector<ToolSpec> tools;
  int64_t ttlMs{0};
};

struct ClientIdentity
{
  std::string name;
  std::string version;
};

class McpClient
{
public:
  static constexpr int kMaxPages = 64;

  McpClient(std::shared_ptr<Transport> transport, ClientIdentity identity);

  [[nodiscard]] Response<ServerDescription> discover() const;

  [[nodiscard]] Response<ToolList> listTools() const;

  [[nodiscard]] Response<ToolOutcome> callTool(const ToolInvocation& invocation) const;

private:
  [[nodiscard]] Response<Json::Value> request(const std::string& method, Json::Value params) const;

  std::shared_ptr<Transport> transport_;
  ClientIdentity identity_;
  mutable std::atomic<int64_t> nextId_{1};
};

}
