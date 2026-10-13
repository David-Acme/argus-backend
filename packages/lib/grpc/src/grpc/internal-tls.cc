#include "internal-tls.hxx"

#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace argus::client
{

namespace
{

struct InstalledTls
{
  std::mutex mutex;
  std::optional<InternalTlsMaterial> material;
};

InstalledTls& installed()
{
  static InstalledTls state;
  return state;
}

std::optional<std::string> readPem(const std::string& path,
                                   std::string_view marker)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::ostringstream content;
  content << input.rdbuf();
  std::string pem = std::move(content).str();
  if (pem.find(marker) == std::string::npos)
    return std::nullopt;
  return pem;
}

}

InternalTlsFiles internalTlsFilesIn(const std::string& directory)
{
  return {.certificate = directory + "/cert.pem",
          .privateKey = directory + "/key.pem",
          .trustBundle = directory + "/ca.pem"};
}

std::optional<InternalTlsMaterial>
readInternalTls(const InternalTlsFiles& files)
{
  auto certificate = readPem(files.certificate, "BEGIN CERTIFICATE");
  auto privateKey = readPem(files.privateKey, "PRIVATE KEY");
  auto trustBundle = readPem(files.trustBundle, "BEGIN CERTIFICATE");
  if (!certificate || !privateKey || !trustBundle)
    return std::nullopt;
  return InternalTlsMaterial{.certificatePem = std::move(*certificate),
                             .privateKeyPem = std::move(*privateKey),
                             .trustBundlePem = std::move(*trustBundle)};
}

std::shared_ptr<grpc::ServerCredentials>
internalServerCredentials(const InternalTlsMaterial& material)
{
  grpc::SslServerCredentialsOptions options(
      GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
  options.pem_root_certs = material.trustBundlePem;
  options.pem_key_cert_pairs.push_back({.private_key = material.privateKeyPem,
                                        .cert_chain = material.certificatePem});
  return grpc::SslServerCredentials(options);
}

std::shared_ptr<grpc::ChannelCredentials>
internalChannelCredentials(const InternalTlsMaterial& material)
{
  return grpc::SslCredentials({.pem_root_certs = material.trustBundlePem,
                               .pem_private_key = material.privateKeyPem,
                               .pem_cert_chain = material.certificatePem});
}

void installInternalTls(std::optional<InternalTlsMaterial> material)
{
  auto& state = installed();
  const std::scoped_lock lock(state.mutex);
  state.material = std::move(material);
}

bool isInternalTlsTarget(std::string_view target)
{
  return target.starts_with(kInternalTlsScheme);
}

ChannelPlan planChannel(const std::string& target)
{
  if (!isInternalTlsTarget(target))
    return {.address = target,
            .credentials = grpc::InsecureChannelCredentials()};
  std::optional<InternalTlsMaterial> material;
  {
    auto& state = installed();
    const std::scoped_lock lock(state.mutex);
    material = state.material;
  }
  if (!material)
    throw std::runtime_error("the gRPC target " + target +
                             " asks for internal TLS, but this process "
                             "installed no internal certificate");
  return {.address = target.substr(kInternalTlsScheme.size()),
          .credentials = internalChannelCredentials(*material)};
}

}
