#include "camera-rpc-server.hxx"

#include <http/listener-config.hxx>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace
{
constexpr uint16_t kDefaultGrpcPort = 7036;
}

struct CameraRpcServer::Impl
{
  explicit Impl(const CameraRpcInput& input)
  {
    if (input.services.empty() ||
        std::ranges::any_of(input.services, [](const grpc::Service* service) {
          return service == nullptr;
        }))
      throw std::invalid_argument("Invalid camera RPC configuration");
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

CameraRpcServer::CameraRpcServer(const CameraRpcInput& input)
    : impl_(std::make_unique<Impl>(input))
{
}

CameraRpcServer::~CameraRpcServer() { shutdown(); }

bool CameraRpcServer::listening() const { return impl_->server_ != nullptr; }

const std::string& CameraRpcServer::address() const { return impl_->address_; }

void CameraRpcServer::shutdown()
{
  if (impl_->server_) {
    impl_->server_->Shutdown();
    impl_->server_.reset();
  }
}
