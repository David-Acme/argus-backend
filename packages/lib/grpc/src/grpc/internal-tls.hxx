#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace argus::client
{

struct InternalTlsFiles
{
  std::string certificate;
  std::string privateKey;
  std::string trustBundle;
};

struct InternalTlsMaterial
{
  std::string certificatePem;
  std::string privateKeyPem;
  std::string trustBundlePem;
};

struct ChannelPlan
{
  std::string address;
  std::shared_ptr<grpc::ChannelCredentials> credentials;
};

struct ListenInput
{
  std::string address;
  std::string tlsAddress;
  std::optional<InternalTlsMaterial> tls;
  std::vector<grpc::Service*> services;
  std::optional<int> maxReceiveBytes;
};

struct ListenPort
{
  std::string address;
  std::shared_ptr<grpc::ServerCredentials> credentials;
  bool mutualTls{false};
};

struct Listener
{
  std::unique_ptr<grpc::Server> server;
  int port{0};
  int tlsPort{0};
};

struct ListenFailureInput
{
  std::string_view name;
  const ListenInput& input;
  const Listener& listener;
  std::string_view consequence;
};

inline constexpr std::string_view kInternalTlsScheme = "tls:";

[[nodiscard]] InternalTlsFiles internalTlsFilesIn(const std::string& directory);

[[nodiscard]] std::optional<InternalTlsMaterial>
readInternalTls(const InternalTlsFiles& files);

[[nodiscard]] std::shared_ptr<grpc::ServerCredentials>
internalServerCredentials(const InternalTlsMaterial& material);

[[nodiscard]] std::shared_ptr<grpc::ChannelCredentials>
internalChannelCredentials(const InternalTlsMaterial& material);

void installInternalTls(InternalTlsMaterial material);

[[nodiscard]] std::optional<InternalTlsMaterial>
installInternalTlsFrom(const std::string& directory);

[[nodiscard]] bool isInternalTlsTarget(std::string_view target);

[[nodiscard]] ChannelPlan openChannel(const std::string& target);

[[nodiscard]] std::vector<ListenPort> listenPorts(const ListenInput& input);

[[nodiscard]] Listener startListening(const ListenInput& input);

[[nodiscard]] std::optional<std::string>
listenFailure(const ListenFailureInput& failure);

}
