#pragma once

#include <mcp/json-rpc.hxx>
#include <mcp/tool.hxx>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace argus::mcp
{

struct ServerIdentity
{
  std::string name;
  std::string version;
  std::string instructions;
};

struct Refusal
{
  std::string code;
  std::string message;
};

class McpServer
{
public:
  using Reply = std::function<void(const ToolOutcome&)>;
  using Handler = std::function<void(const ToolInvocation&, Reply)>;
  using SyncHandler = std::function<ToolOutcome(const ToolInvocation&)>;
  using Gate = std::function<std::optional<Refusal>(const ToolInvocation&, const ToolSpec&)>;
  using Done = std::function<void(std::string)>;

  static constexpr std::chrono::milliseconds kBlockingTimeout{30000};

  explicit McpServer(ServerIdentity identity);

  void add(ToolSpec spec, Handler handler);

  void addSync(ToolSpec spec, SyncHandler handler);

  void setGate(Gate gate);

  void handle(std::string_view frame, const Done& done) const;

  [[nodiscard]] std::string handleBlocking(std::string_view frame,
                                           std::chrono::milliseconds timeout = kBlockingTimeout) const;

  [[nodiscard]] std::vector<ToolSpec> tools() const;

  [[nodiscard]] const ToolSpec* find(std::string_view name) const;

private:
  struct Entry
  {
    ToolSpec spec;
    Handler handler;
  };

  struct Exchange
  {
    const RpcRequest& request;
    const Done& done;
  };

  void dispatch(const Exchange& exchange) const;
  void discover(const Exchange& exchange) const;
  void list(const Exchange& exchange) const;
  void call(const Exchange& exchange) const;
  void run(const Entry& entry, const ToolInvocation& invocation, const Exchange& exchange) const;
  [[nodiscard]] Json::Value serverMeta() const;

  ServerIdentity identity_;
  std::map<std::string, Entry, std::less<>> entries_;
  Gate gate_;
};

}
