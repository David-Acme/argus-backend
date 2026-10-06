#include "mcp-rpc.hxx"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace argus::mcp
{

namespace
{
grpc::ServerUnaryReactor* finish(grpc::CallbackServerContext* context, const grpc::Status& status)
{
  auto* reactor = context->DefaultReactor();
  reactor->Finish(status);
  return reactor;
}
}

McpRpcService::McpRpcService(McpRpcInput input) : input_(std::move(input))
{
  if (!input_.server)
    throw std::invalid_argument("An MCP surface needs a tool server");
  if (!input_.gate || input_.gate->open() || input_.gate->pairedCount() == 0)
    throw std::invalid_argument("An MCP surface needs a paired caller credential");
  if (input_.callers.empty())
    throw std::invalid_argument("An MCP surface needs at least one admitted caller");
}

grpc::ServerUnaryReactor* McpRpcService::Rpc(grpc::CallbackServerContext* context,
                                             const v1::Frame* request,
                                             v1::Frame* response)
{
  const std::vector<std::string_view> allowed(input_.callers.begin(), input_.callers.end());
  const auto admission = input_.gate->admit(context, allowed);
  if (!admission.admitted())
    return finish(context, argus::client::FleetCallerGate::refusal(admission.verdict));
  if (request->payload().size() > kMaxFrameBytes)
    return finish(context, {grpc::StatusCode::INVALID_ARGUMENT, "The frame is too large"});
  auto* reactor = context->DefaultReactor();
  input_.server->handle(request->payload(), [reactor, response](std::string frame) {
    response->set_payload(std::move(frame));
    reactor->Finish(grpc::Status::OK);
  });
  return reactor;
}

}
