#pragma once

#include <atomic>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <thread>

class GrpcServerDrain
{
public:
  static constexpr std::chrono::milliseconds kShutdownGrace{2000};

  explicit GrpcServerDrain(grpc::Server* server) : server_(server) {}
  ~GrpcServerDrain();

  GrpcServerDrain(const GrpcServerDrain&) = delete;
  GrpcServerDrain& operator=(const GrpcServerDrain&) = delete;

  void requestStop();
  [[nodiscard]] bool drained() const;
  void stopAndWait();

private:
  grpc::Server* server_{nullptr};
  std::mutex mutex_;
  std::thread worker_;
  std::atomic<bool> drained_{false};
};
