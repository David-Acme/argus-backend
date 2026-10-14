#pragma once

#include <feature/vlm/services/vision-service.hxx>
#include <vlm/vlm-client.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/impl/service_type.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct VlmRpcInput
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::function<argus::vlm::Capabilities()> capabilities;
  std::function<std::string(const VisionDescribeMatInput&)> describe;
  int slots{1};
  std::vector<grpc::Service*> services;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
};

class VlmRpcServer
{
public:
  explicit VlmRpcServer(VlmRpcInput input);
  ~VlmRpcServer();
  [[nodiscard]] int port() const;
  [[nodiscard]] int tlsPort() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
