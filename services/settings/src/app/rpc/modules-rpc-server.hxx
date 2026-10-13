#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ModulesRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

struct ModulesRpcPort
{
  std::string address;
  std::shared_ptr<grpc::ServerCredentials> credentials;
  bool mutualTls{false};
};

struct ModulesRpcListener
{
  std::unique_ptr<grpc::Server> server;
  int port{0};
  int tlsPort{0};
};

[[nodiscard]] std::vector<ModulesRpcPort>
modulesRpcPorts(const ModulesRpcListenInput& input);

[[nodiscard]] ModulesRpcListener
startModulesRpc(const ModulesRpcListenInput& input);
