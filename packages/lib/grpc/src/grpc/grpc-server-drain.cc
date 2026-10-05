#include "grpc-server-drain.hxx"

#include <utility>

namespace argus::client
{
GrpcServerDrain::GrpcServerDrain(std::unique_ptr<grpc::Server> server,
                                 std::chrono::milliseconds deadline)
    : server_(std::move(server)), deadline_(deadline)
{
}

GrpcServerDrain::~GrpcServerDrain() { stop(); }

void GrpcServerDrain::requestStop()
{
  const std::scoped_lock lock(mutex_);
  if (stopper_.joinable() || finished_.load(std::memory_order_acquire))
    return;
  if (!server_) {
    finished_.store(true, std::memory_order_release);
    return;
  }
  stopper_ = std::thread([this] {
    server_->Shutdown(std::chrono::system_clock::now() + deadline_);
    finished_.store(true, std::memory_order_release);
  });
}

bool GrpcServerDrain::drained() const
{
  return finished_.load(std::memory_order_acquire);
}

void GrpcServerDrain::stop()
{
  requestStop();
  const std::scoped_lock lock(mutex_);
  if (stopper_.joinable())
    stopper_.join();
  server_.reset();
}
}
