#pragma once

#include <feature/spot/services/keyword-spotter.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <stt/stt-client.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/impl/service_type.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct SttRpcInput
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  std::function<argus::stt::Capabilities()> capabilities;
  std::function<bool(const std::string&)> acceptsLanguage;
  std::function<std::string(const TranscribeRequest&)> transcribe;
  int slots{1};
  std::vector<grpc::Service*> services;
  std::string tlsAddress;
  std::optional<argus::client::InternalTlsMaterial> tls;
  std::function<std::unique_ptr<SpotSession>()> spot;
};

class SttRpcServer
{
public:
  explicit SttRpcServer(SttRpcInput input);
  ~SttRpcServer();
  [[nodiscard]] int port() const;
  [[nodiscard]] int tlsPort() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
