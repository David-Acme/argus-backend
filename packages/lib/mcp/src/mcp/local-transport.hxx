#pragma once

#include <mcp/client.hxx>
#include <mcp/server.hxx>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace argus::mcp
{

class LocalTransport final : public Transport
{
public:
  explicit LocalTransport(std::shared_ptr<const McpServer> server) : server_(std::move(server)) {}

  [[nodiscard]] std::optional<std::string> exchange(const std::string& frame) override
  {
    return server_->handleBlocking(frame);
  }

private:
  std::shared_ptr<const McpServer> server_;
};

}
