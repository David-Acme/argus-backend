#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ProductivityRpcInput
{
  std::vector<grpc::Service*> services;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
};

struct ProductivityRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

struct ProductivityRpcPort
{
  std::string address;
  std::shared_ptr<grpc::ServerCredentials> credentials;
  bool mutualTls{false};
};

struct ProductivityRpcListener
{
  std::unique_ptr<grpc::Server> server;
  int port{0};
  int tlsPort{0};
};

[[nodiscard]] std::vector<ProductivityRpcPort>
productivityRpcPorts(const ProductivityRpcListenInput& input);

[[nodiscard]] ProductivityRpcListener
startProductivityRpc(const ProductivityRpcListenInput& input);

class ProductivityRpcServer
{
public:
  explicit ProductivityRpcServer(const ProductivityRpcInput& input);
  ~ProductivityRpcServer();
  [[nodiscard]] bool listening() const;
  [[nodiscard]] const std::string& address() const;
  [[nodiscard]] int tlsPort() const;
  void requestStop();
  [[nodiscard]] bool drained() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
