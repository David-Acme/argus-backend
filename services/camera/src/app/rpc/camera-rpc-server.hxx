#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>

struct CameraRpcInput
{
  std::vector<grpc::Service*> services;
};

class CameraRpcServer
{
public:
  explicit CameraRpcServer(const CameraRpcInput& input);
  ~CameraRpcServer();
  [[nodiscard]] bool listening() const;
  [[nodiscard]] const std::string& address() const;
  void shutdown();
  void requestStop();
  [[nodiscard]] bool drained() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
