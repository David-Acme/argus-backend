#include "internal-tls.hxx"

#include <algorithm>
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
  bool channelOpened{false};
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

void installInternalTls(InternalTlsMaterial material)
{
  if (material.certificatePem.empty() || material.privateKeyPem.empty() ||
      material.trustBundlePem.empty())
    throw std::invalid_argument("internal TLS material needs a certificate, "
                                "a private key and a trust bundle");
  auto& state = installed();
  const std::scoped_lock lock(state.mutex);
  if (state.material)
    throw std::logic_error("internal TLS is installed once per process");
  if (state.channelOpened)
    throw std::logic_error("internal TLS must be installed before the "
                           "process opens its first gRPC channel");
  state.material = std::move(material);
}

std::optional<InternalTlsMaterial>
installInternalTlsFrom(const std::string& directory)
{
  if (directory.empty())
    return std::nullopt;
  auto material = readInternalTls(internalTlsFilesIn(directory));
  if (!material)
    throw std::runtime_error("the internal TLS directory " + directory +
                             " does not hold a readable cert.pem, key.pem "
                             "and ca.pem");
  installInternalTls(*material);
  return material;
}

bool isInternalTlsTarget(std::string_view target)
{
  return target.starts_with(kInternalTlsScheme);
}

ChannelPlan openChannel(const std::string& target)
{
  std::optional<InternalTlsMaterial> material;
  {
    auto& state = installed();
    const std::scoped_lock lock(state.mutex);
    state.channelOpened = true;
    if (isInternalTlsTarget(target))
      material = state.material;
  }
  if (!isInternalTlsTarget(target))
    return {.address = target,
            .credentials = grpc::InsecureChannelCredentials()};
  if (!material)
    throw std::runtime_error("the gRPC target " + target +
                             " asks for internal TLS, but this process "
                             "installed no internal certificate");
  return {.address = target.substr(kInternalTlsScheme.size()),
          .credentials = internalChannelCredentials(*material)};
}

std::vector<ListenPort> listenPorts(const ListenInput& input)
{
  std::vector<ListenPort> ports;
  if (input.address.empty())
    return ports;
  ports.push_back({.address = input.address,
                   .credentials = grpc::InsecureServerCredentials(),
                   .mutualTls = false});
  if (input.tls && !input.tlsAddress.empty())
    ports.push_back({.address = input.tlsAddress,
                     .credentials = internalServerCredentials(*input.tls),
                     .mutualTls = true});
  return ports;
}

Listener startListening(const ListenInput& input)
{
  Listener listener;
  const auto ports = listenPorts(input);
  if (ports.empty())
    return listener;
  grpc::ServerBuilder builder;
  for (const auto& port : ports)
    builder.AddListeningPort(port.address, port.credentials,
                             port.mutualTls ? &listener.tlsPort
                                            : &listener.port);
  for (grpc::Service* service : input.services)
    builder.RegisterService(service);
  listener.server = builder.BuildAndStart();
  const bool tlsRequested = std::ranges::any_of(ports, &ListenPort::mutualTls);
  if (listener.server &&
      (listener.port == 0 || (tlsRequested && listener.tlsPort == 0))) {
    listener.server->Shutdown();
    listener.server.reset();
  }
  if (!listener.server) {
    listener.port = 0;
    listener.tlsPort = 0;
  }
  return listener;
}

}
