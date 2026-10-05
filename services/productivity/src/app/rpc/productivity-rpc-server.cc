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

std::unique_ptr<grpc::Server> buildServer(const ProductivityRpcInput& input,
                                          const std::string& address)
{
  if (input.services.empty() ||
      std::ranges::any_of(input.services, [](const grpc::Service* service) {
        return service == nullptr;
      }))
    throw std::invalid_argument("Invalid productivity RPC configuration");
  grpc::ServerBuilder builder;
  builder.AddListeningPort(address, grpc::InsecureServerCredentials());
  for (grpc::Service* service : input.services)
    builder.RegisterService(service);
  return builder.BuildAndStart();
}
}

struct ProductivityRpcServer::Impl
{
  explicit Impl(const ProductivityRpcInput& input) : address_(listenerAddress())
  {
    auto server = buildServer(input, address_);
    listening_ = server != nullptr;
    drain_ = std::make_unique<argus::client::GrpcServerDrain>(std::move(server), kDrainDeadline);
  }

  std::string address_;
  bool listening_{false};
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
