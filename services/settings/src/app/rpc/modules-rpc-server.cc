#include "modules-rpc-server.hxx"

#include <algorithm>

std::vector<ModulesRpcPort> modulesRpcPorts(const ModulesRpcListenInput& input)
{
  std::vector<ModulesRpcPort> ports;
  if (input.address.empty())
    return ports;
  ports.push_back({.address = input.address,
                   .credentials = grpc::InsecureServerCredentials(),
                   .mutualTls = false});
  if (input.tls && !input.tlsAddress.empty())
    ports.push_back(
        {.address = input.tlsAddress,
         .credentials = argus::client::internalServerCredentials(*input.tls),
         .mutualTls = true});
  return ports;
}

ModulesRpcListener startModulesRpc(const ModulesRpcListenInput& input)
{
  ModulesRpcListener listener;
  const auto ports = modulesRpcPorts(input);
  if (ports.empty())
    return listener;
  grpc::ServerBuilder builder;
  for (const auto& port : ports)
    builder.AddListeningPort(port.address, port.credentials,
                             port.mutualTls ? &listener.tlsPort
                                            : &listener.port);
  for (grpc::Service* service : input.services)
    builder.RegisterService(service);
  listener.server = builder.BuildAndStart();
  const bool tlsRequested =
      std::ranges::any_of(ports, &ModulesRpcPort::mutualTls);
  if (listener.server &&
      (listener.port == 0 || (tlsRequested && listener.tlsPort == 0))) {
    listener.server->Shutdown();
    listener.server.reset();
  }
  if (!listener.server) {
    listener.port = 0;
    listener.tlsPort = 0;
  }
  return listener;
}
