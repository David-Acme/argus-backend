#include "grpc-server-drain.hxx"

GrpcServerDrain::GrpcServerDrain(grpc::Server* server,
                                 std::chrono::milliseconds deadline)
    : server_(server), deadline_(deadline)
{
  if (server_ == nullptr)
    done_.store(true, std::memory_order_release);
}

GrpcServerDrain::~GrpcServerDrain()
{
  finish();
}

void GrpcServerDrain::requestStop()
{
  std::scoped_lock lock(mutex_);
  if (worker_.joinable() || done_.load(std::memory_order_acquire))
    return;
  worker_ = std::thread([this]() {
    server_->Shutdown(std::chrono::system_clock::now() + deadline_);
    done_.store(true, std::memory_order_release);
  });
}

bool GrpcServerDrain::drained() const
{
  return done_.load(std::memory_order_acquire);
}

void GrpcServerDrain::finish()
{
  requestStop();
  std::scoped_lock lock(mutex_);
  if (worker_.joinable())
    worker_.join();
}
