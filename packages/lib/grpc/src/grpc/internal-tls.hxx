#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

inline constexpr std::string_view kInternalTlsScheme = "tls:";

[[nodiscard]] InternalTlsFiles internalTlsFilesIn(const std::string& directory);

[[nodiscard]] std::optional<InternalTlsMaterial>
readInternalTls(const InternalTlsFiles& files);

[[nodiscard]] std::shared_ptr<grpc::ServerCredentials>
internalServerCredentials(const InternalTlsMaterial& material);

[[nodiscard]] std::shared_ptr<grpc::ChannelCredentials>
internalChannelCredentials(const InternalTlsMaterial& material);

void installInternalTls(std::optional<InternalTlsMaterial> material);

[[nodiscard]] bool isInternalTlsTarget(std::string_view target);

[[nodiscard]] ChannelPlan planChannel(const std::string& target);

}
