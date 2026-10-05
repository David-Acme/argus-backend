#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

class GrpcServerDrain
{
public:
  GrpcServerDrain(std::unique_ptr<grpc::Server> server,
                  std::chrono::milliseconds deadline);
  ~GrpcServerDrain();

  GrpcServerDrain(const GrpcServerDrain&) = delete;
  GrpcServerDrain& operator=(const GrpcServerDrain&) = delete;

  void requestStop();

  [[nodiscard]] bool drained() const;

  void stop();

private:
  std::unique_ptr<grpc::Server> server_;
  std::chrono::milliseconds deadline_;
  std::mutex mutex_;
  std::thread stopper_;
  std::atomic<bool> finished_{false};
};
