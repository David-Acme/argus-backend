#pragma once

#include <mcp/client.hxx>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace argus::mcp
{

struct ToolEndpoint
{
  std::string target;
  std::string credential;
  std::chrono::milliseconds timeout{15000};
};

class GrpcToolTransport final : public Transport
{
public:
  explicit GrpcToolTransport(ToolEndpoint endpoint);
  ~GrpcToolTransport() override;

  [[nodiscard]] std::optional<std::string> exchange(const std::string& frame) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
