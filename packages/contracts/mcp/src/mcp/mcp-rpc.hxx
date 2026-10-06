#pragma once

#include <grpc/fleet-caller-gate.hxx>
#include <mcp.grpc.pb.h>
#include <mcp/server.hxx>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace argus::mcp
{

inline constexpr std::string_view kToolCaller = "llm";
inline constexpr std::size_t kMaxFrameBytes = 262144;

struct McpRpcInput
{
  std::shared_ptr<const McpServer> server;
  std::shared_ptr<const argus::client::FleetCallerGate> gate;
  std::vector<std::string> callers{std::string(kToolCaller)};
};

class McpRpcService final : public v1::Mcp::CallbackService
{
public:
  explicit McpRpcService(McpRpcInput input);

  grpc::ServerUnaryReactor* Rpc(grpc::CallbackServerContext* context,
                                const v1::Frame* request,
                                v1::Frame* response) override;

private:
  McpRpcInput input_;
};

}
