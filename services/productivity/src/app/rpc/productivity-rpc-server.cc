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
constexpr std::chrono::milliseconds kDrainDeadline{2000};

std::string listenerAddress()
{
  return GrpcListenerConfig::resolve().address();
}

argus::client::ListenInput
listenInputOf(const ProductivityRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}
}

argus::client::Listener
startProductivityRpc(const ProductivityRpcListenInput& input)
{
  if (input.services.empty() ||
      std::ranges::any_of(input.services, [](const grpc::Service* service) {
        return service == nullptr;
      }))
    throw std::invalid_argument("Invalid productivity RPC configuration");
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
productivityRpcFailure(const ProductivityRpcListenInput& input,
                       const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "gRPC server",
       .input = listening,
       .listener = listener,
       .consequence = "without it sync cannot pull the productivity tables, "
                      "the llm cannot create or list reminders or use the "
                      "calendar, project and task tools, and settings cannot "
                      "reach productivity's settings, module data or module "
                      "impact"});
}

struct ProductivityRpcServer::Impl
{
  explicit Impl(const ProductivityRpcInput& input) : address_(listenerAddress())
  {
    const ProductivityRpcListenInput listen{.address = address_,
                                            .tlsAddress = input.tlsAddress,
                                            .tls = input.tls,
                                            .services = input.services};
    auto listener = startProductivityRpc(listen);
    failure_ = productivityRpcFailure(listen, listener);
    tlsPort_ = listener.tlsPort;
    drain_ = std::make_unique<argus::client::GrpcServerDrain>(
        std::move(listener.server), kDrainDeadline);
  }

  std::string address_;
  std::optional<std::string> failure_;
  int tlsPort_{0};
  std::unique_ptr<argus::client::GrpcServerDrain> drain_;
};

ProductivityRpcServer::ProductivityRpcServer(const ProductivityRpcInput& input)
    : impl_(std::make_unique<Impl>(input))
{
}

ProductivityRpcServer::~ProductivityRpcServer() { shutdown(); }

const std::optional<std::string>& ProductivityRpcServer::failure() const
{
  return impl_->failure_;
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
