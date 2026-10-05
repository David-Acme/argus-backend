#pragma once

#include <atomic>
#include <chrono>
#include <grpcpp/server.h>
#include <mutex>
#include <thread>

class GrpcServerDrain
{
public:
  GrpcServerDrain(grpc::Server* server, std::chrono::milliseconds deadline);
  ~GrpcServerDrain();

  GrpcServerDrain(const GrpcServerDrain&) = delete;
  GrpcServerDrain& operator=(const GrpcServerDrain&) = delete;
  GrpcServerDrain(GrpcServerDrain&&) = delete;
  GrpcServerDrain& operator=(GrpcServerDrain&&) = delete;

  void requestStop();
  [[nodiscard]] bool drained() const;
  void finish();

private:
  grpc::Server* const server_;
  const std::chrono::milliseconds deadline_;
  std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> done_{false};
};
