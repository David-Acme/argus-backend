#include "camera-rpc-server.hxx"

#include <http/listener-config.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
constexpr uint16_t kDefaultGrpcPort = 7036;
constexpr auto kShutdownGrace = std::chrono::seconds(2);
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
  std::mutex stopMutex_;
  std::jthread stopper_;
  std::atomic<bool> stopped_{false};
};

CameraRpcServer::CameraRpcServer(const CameraRpcInput& input)
    : impl_(std::make_unique<Impl>(input))
{
}

CameraRpcServer::~CameraRpcServer() { shutdown(); }

bool CameraRpcServer::listening() const { return impl_->server_ != nullptr; }

const std::string& CameraRpcServer::address() const { return impl_->address_; }

void CameraRpcServer::requestStop()
{
  std::scoped_lock lock(impl_->stopMutex_);
  if (impl_->stopper_.joinable() || !impl_->server_) {
    if (!impl_->server_)
      impl_->stopped_.store(true, std::memory_order_release);
    return;
  }
  impl_->stopper_ = std::jthread([impl = impl_.get()]() {
    impl->server_->Shutdown(std::chrono::system_clock::now() + kShutdownGrace);
    impl->stopped_.store(true, std::memory_order_release);
  });
}

bool CameraRpcServer::drained() const
{
  return impl_->stopped_.load(std::memory_order_acquire);
}

void CameraRpcServer::shutdown()
{
  requestStop();
  {
    std::scoped_lock lock(impl_->stopMutex_);
    if (impl_->stopper_.joinable())
      impl_->stopper_.join();
  }
  impl_->server_.reset();
}
