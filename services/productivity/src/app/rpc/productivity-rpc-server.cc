#include "productivity-rpc-server.hxx"

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
constexpr uint16_t kDefaultGrpcPort = 7037;
constexpr std::chrono::milliseconds kDrainDeadline{2000};
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
  std::mutex mutex_;
  std::thread stopper_;
  std::atomic<bool> finished_{false};
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

void ProductivityRpcServer::requestStop()
{
  const std::scoped_lock lock(impl_->mutex_);
  if (impl_->stopper_.joinable())
    return;
  if (!impl_->server_) {
    impl_->finished_.store(true, std::memory_order_release);
    return;
  }
  impl_->stopper_ = std::thread([impl = impl_.get()] {
    impl->server_->Shutdown(std::chrono::system_clock::now() + kDrainDeadline);
    impl->finished_.store(true, std::memory_order_release);
  });
}

bool ProductivityRpcServer::drained() const
{
  return impl_->finished_.load(std::memory_order_acquire);
}

void ProductivityRpcServer::shutdown()
{
  requestStop();
  const std::scoped_lock lock(impl_->mutex_);
  if (impl_->stopper_.joinable())
    impl_->stopper_.join();
  impl_->server_.reset();
}
