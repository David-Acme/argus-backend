#include "productivity-rpc-server.hxx"

#include <http/listener-config.hxx>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace
{
constexpr uint16_t kDefaultGrpcPort = 7037;
}

struct ProductivityRpcServer::Impl
{
  explicit Impl(const ProductivityRpcInput& input)
  {
    if (input.services.empty() ||
        std::ranges::any_of(input.services, [](const grpc::Service* service) {
          return service == nullptr;
        }))
      throw std::invalid_argument("Invalid productivity RPC configuration");
    const GrpcListenerConfig listener =
        GrpcListenerConfig::resolve(kDefaultGrpcPort);
    address_ = listener.host + ":" + std::to_string(listener.port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    for (grpc::Service* service : input.services)
      builder.RegisterService(service);
    server_ = builder.BuildAndStart();
  }

  std::string address_;
  std::unique_ptr<grpc::Server> server_;
};

ProductivityRpcServer::ProductivityRpcServer(const ProductivityRpcInput& input)
    : impl_(std::make_unique<Impl>(input))
{
}

ProductivityRpcServer::~ProductivityRpcServer() { shutdown(); }

bool ProductivityRpcServer::listening() const
{
  return impl_->server_ != nullptr;
}

const std::string& ProductivityRpcServer::address() const
{
  return impl_->address_;
}

void ProductivityRpcServer::shutdown()
{
  if (impl_->server_) {
    impl_->server_->Shutdown();
    impl_->server_.reset();
  }
}
