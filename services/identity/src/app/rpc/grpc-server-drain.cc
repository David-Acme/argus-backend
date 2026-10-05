#include "grpc-server-drain.hxx"

GrpcServerDrain::~GrpcServerDrain()
{
  stopAndWait();
}

void GrpcServerDrain::requestStop()
{
  const std::scoped_lock lock(mutex_);
  if (worker_.joinable() || drained_.load())
    return;
  if (server_ == nullptr) {
    drained_.store(true);
    return;
  }
  worker_ = std::thread([this] {
    server_->Shutdown(std::chrono::system_clock::now() + kShutdownGrace);
    drained_.store(true);
  });
}

bool GrpcServerDrain::drained() const
{
  return drained_.load();
}

void GrpcServerDrain::stopAndWait()
{
  requestStop();
  const std::scoped_lock lock(mutex_);
  if (worker_.joinable())
    worker_.join();
}
