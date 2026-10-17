#include "camera-rpc-server.hxx"

#include <http/listener-config.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
constexpr auto kShutdownGrace = std::chrono::seconds(2);

argus::client::ListenInput listenInputOf(const CameraRpcListenInput& input)
{
  return {.address = input.address,
          .tlsAddress = input.tlsAddress,
          .tls = input.tls,
          .services = input.services,
          .maxReceiveBytes = std::nullopt};
}
}

argus::client::Listener startCameraRpc(const CameraRpcListenInput& input)
{
  if (input.services.empty() ||
      std::ranges::any_of(input.services, [](const grpc::Service* service) {
        return service == nullptr;
      }))
    throw std::invalid_argument("Invalid camera RPC configuration");
  return argus::client::startListening(listenInputOf(input));
}

std::optional<std::string>
cameraRpcFailure(const CameraRpcListenInput& input,
                 const argus::client::Listener& listener)
{
  const auto listening = listenInputOf(input);
  return argus::client::listenFailure(
      {.name = "gRPC server",
       .input = listening,
       .listener = listener,
       .consequence = "without it sync cannot pull the camera tables, guard "
                      "cannot announce, listen, sound the alarm or the siren, "
                      "or take a person crop, the llm cannot read the camera "
                      "catalog or use the camera tools, and settings cannot "
                      "reach camera's settings or module data"});
}

struct CameraRpcServer::Impl
{
  explicit Impl(const CameraRpcInput& input)
  {
    address_ = GrpcListenerConfig::resolve().address();
    const CameraRpcListenInput listen{.address = address_,
                                      .tlsAddress = input.tlsAddress,
                                      .tls = input.tls,
                                      .services = input.services};
    auto started = startCameraRpc(listen);
    failure_ = cameraRpcFailure(listen, started);
    tlsPort_ = started.tlsPort;
    server_ = std::move(started.server);
  }

  std::string address_;
  std::optional<std::string> failure_;
  int tlsPort_{0};
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

const std::optional<std::string>& CameraRpcServer::failure() const
{
  return impl_->failure_;
}

int CameraRpcServer::tlsPort() const { return impl_->tlsPort_; }

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
