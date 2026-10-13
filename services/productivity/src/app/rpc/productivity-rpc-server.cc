#include "productivity-rpc-server.hxx"

#include <grpc/grpc-server-drain.hxx>
#include <http/listener-config.hxx>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace
{
constexpr uint16_t kDefaultGrpcPort = 7037;
constexpr std::chrono::milliseconds kDrainDeadline{2000};

std::string listenerAddress()
{
  const GrpcListenerConfig listener =
      GrpcListenerConfig::resolve(kDefaultGrpcPort);
  return listener.host + ":" + std::to_string(listener.port);
}
}

std::vector<ProductivityRpcPort>
productivityRpcPorts(const ProductivityRpcListenInput& input)
{
  std::vector<ProductivityRpcPort> ports;
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

ProductivityRpcListener
startProductivityRpc(const ProductivityRpcListenInput& input)
{
  ProductivityRpcListener listener;
  const auto ports = productivityRpcPorts(input);
  if (ports.empty())
    return listener;
  if (input.services.empty() ||
      std::ranges::any_of(input.services, [](const grpc::Service* service) {
        return service == nullptr;
      }))
    throw std::invalid_argument("Invalid productivity RPC configuration");
  grpc::ServerBuilder builder;
  for (const auto& port : ports)
    builder.AddListeningPort(port.address, port.credentials,
                             port.mutualTls ? &listener.tlsPort
                                            : &listener.port);
  for (grpc::Service* service : input.services)
    builder.RegisterService(service);
  listener.server = builder.BuildAndStart();
  const bool tlsRequested =
      std::ranges::any_of(ports, &ProductivityRpcPort::mutualTls);
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

struct ProductivityRpcServer::Impl
{
  explicit Impl(const ProductivityRpcInput& input) : address_(listenerAddress())
  {
    auto listener = startProductivityRpc({.address = address_,
                                          .tlsAddress = input.tlsAddress,
                                          .tls = input.tls,
                                          .services = input.services});
    listening_ = listener.server != nullptr;
    tlsPort_ = listener.tlsPort;
    drain_ = std::make_unique<argus::client::GrpcServerDrain>(
        std::move(listener.server), kDrainDeadline);
  }

  std::string address_;
  bool listening_{false};
  int tlsPort_{0};
  std::unique_ptr<argus::client::GrpcServerDrain> drain_;
};

ProductivityRpcServer::ProductivityRpcServer(const ProductivityRpcInput& input)
    : impl_(std::make_unique<Impl>(input))
{
}

ProductivityRpcServer::~ProductivityRpcServer() { shutdown(); }

bool ProductivityRpcServer::listening() const
{
  return impl_->listening_;
}

const std::string& ProductivityRpcServer::address() const
{
  return impl_->address_;
}

int ProductivityRpcServer::tlsPort() const
{
  return impl_->tlsPort_;
}

void ProductivityRpcServer::requestStop()
{
  impl_->drain_->requestStop();
}

bool ProductivityRpcServer::drained() const
{
  return impl_->drain_->drained();
}

void ProductivityRpcServer::shutdown()
{
  impl_->drain_->stop();
}
