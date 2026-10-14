#pragma once

#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct CameraRpcInput
{
  std::vector<grpc::Service*> services;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
};

struct CameraRpcListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
};

[[nodiscard]] argus::client::Listener
startCameraRpc(const CameraRpcListenInput& input);

[[nodiscard]] std::optional<std::string>
cameraRpcFailure(const CameraRpcListenInput& input,
                 const argus::client::Listener& listener);

class CameraRpcServer
{
public:
  explicit CameraRpcServer(const CameraRpcInput& input);
  ~CameraRpcServer();
  [[nodiscard]] const std::optional<std::string>& failure() const;
  [[nodiscard]] const std::string& address() const;
  [[nodiscard]] int tlsPort() const;
  void shutdown();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
