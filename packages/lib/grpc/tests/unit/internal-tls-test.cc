#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <grpc/grpc-client-base.hxx>
#include <grpc/health/v1/health.grpc.pb.h>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

using argus::client::InternalTlsMaterial;

class AlwaysServing final : public grpc::health::v1::Health::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Check(grpc::CallbackServerContext* context,
        const grpc::health::v1::HealthCheckRequest*,
        grpc::health::v1::HealthCheckResponse* response) override
  {
    response->set_status(grpc::health::v1::HealthCheckResponse::SERVING);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

class ServingOn
{
public:
  explicit ServingOn(std::shared_ptr<grpc::ServerCredentials> credentials)
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", std::move(credentials), &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    if (!server_ || port_ <= 0)
      throw std::runtime_error("the test server did not start");
  }

  ~ServingOn()
  {
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
  }

  ServingOn(const ServingOn&) = delete;
  ServingOn& operator=(const ServingOn&) = delete;
  ServingOn(ServingOn&&) = delete;
  ServingOn& operator=(ServingOn&&) = delete;

  [[nodiscard]] std::string address(const std::string& host) const
  {
    return host + ":" + std::to_string(port_);
  }

private:
  AlwaysServing service_;
  int port_ = 0;
  std::unique_ptr<grpc::Server> server_;
};

std::string environment(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::runtime_error(std::string(name) +
                             " is not set; run this test through "
                             "scripts/internal-tls-test.sh");
  return value;
}

std::string serviceDirectory(const std::string& service)
{
  return environment("ARGUS_INTERNAL_TLS_PKI") + "/" + service;
}

InternalTlsMaterial materialOf(const std::string& directory)
{
  auto material = argus::client::readInternalTls(
      argus::client::internalTlsFilesIn(directory));
  if (!material)
    throw std::runtime_error("no internal TLS material in " + directory);
  return *std::move(material);
}

grpc::StatusCode healthOver(const std::shared_ptr<grpc::Channel>& channel)
{
  auto stub = grpc::health::v1::Health::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  const grpc::health::v1::HealthCheckRequest request;
  grpc::health::v1::HealthCheckResponse response;
  return stub->Check(&context, request, &response).error_code();
}

grpc::StatusCode healthCarrying(const std::shared_ptr<grpc::Channel>& channel,
                                std::size_t bytes)
{
  auto stub = grpc::health::v1::Health::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(5));
  grpc::health::v1::HealthCheckRequest request;
  request.set_service(std::string(bytes, 'x'));
  grpc::health::v1::HealthCheckResponse response;
  return stub->Check(&context, request, &response).error_code();
}

std::shared_ptr<grpc::Channel>
authChannelOn(int port,
              const std::shared_ptr<grpc::ChannelCredentials>& credentials)
{
  grpc::ChannelArguments arguments;
  arguments.SetSslTargetNameOverride("argus-auth");
  return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
                                   credentials, arguments);
}

std::shared_ptr<grpc::Channel>
channelNamed(const ServingOn& server,
             const std::shared_ptr<grpc::ChannelCredentials>& credentials,
             const std::string& name)
{
  grpc::ChannelArguments arguments;
  arguments.SetSslTargetNameOverride(name);
  return grpc::CreateCustomChannel(server.address("127.0.0.1"), credentials,
                                   arguments);
}

}

TEST_CASE("a service directory holds its certificate and its key and the "
          "trust bundle")
{
  const auto files = argus::client::internalTlsFilesIn("/run/secrets/tls");
  CHECK(files.certificate == "/run/secrets/tls/cert.pem");
  CHECK(files.privateKey == "/run/secrets/tls/key.pem");
  CHECK(files.trustBundle == "/run/secrets/tls/ca.pem");
}

TEST_CASE("material is read only when every file holds what it should")
{
  const auto material = argus::client::readInternalTls(
      argus::client::internalTlsFilesIn(serviceDirectory("auth")));
  CHECK(material.has_value());
  const auto read = material.value_or(InternalTlsMaterial{});
  CHECK(read.certificatePem.find("BEGIN CERTIFICATE") != std::string::npos);
  CHECK(read.privateKeyPem.find("PRIVATE KEY") != std::string::npos);
  CHECK(read.trustBundlePem.find("BEGIN CERTIFICATE") != std::string::npos);

  CHECK_FALSE(argus::client::readInternalTls(
                  argus::client::internalTlsFilesIn("/nonexistent/argus"))
                  .has_value());

  const auto swapped =
      std::filesystem::temp_directory_path() / "argus-internal-tls-swapped";
  std::filesystem::create_directories(swapped);
  std::filesystem::copy_file(serviceDirectory("auth") + "/cert.pem",
                             swapped / "cert.pem",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(serviceDirectory("auth") + "/cert.pem",
                             swapped / "key.pem",
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(serviceDirectory("auth") + "/ca.pem",
                             swapped / "ca.pem",
                             std::filesystem::copy_options::overwrite_existing);
  CHECK_FALSE(argus::client::readInternalTls(
                  argus::client::internalTlsFilesIn(swapped.string()))
                  .has_value());
  std::ofstream(swapped / "key.pem", std::ios::trunc).flush();
  CHECK_FALSE(argus::client::readInternalTls(
                  argus::client::internalTlsFilesIn(swapped.string()))
                  .has_value());
  std::filesystem::remove_all(swapped);
}

TEST_CASE("a tls target with nothing installed throws while a plain target "
          "needs nothing")
{
  CHECK_FALSE(argus::client::isInternalTlsTarget("argus-identity:7040"));
  CHECK(argus::client::isInternalTlsTarget("tls:argus-identity:7240"));
  CHECK(argus::client::makeChannel("argus-identity:7040") != nullptr);
  CHECK_THROWS_AS(argus::client::makeChannel("tls:argus-identity:7240"),
                  std::runtime_error);
  CHECK_THROWS_AS(argus::client::makeStreamingChannel(
                      "tls:argus-identity:7240"),
                  std::runtime_error);
}

TEST_CASE("two services speak mutual TLS through makeChannel")
{
  argus::client::installInternalTls(materialOf(serviceDirectory("camera")));
  const ServingOn auth(argus::client::internalServerCredentials(
      materialOf(serviceDirectory("auth"))));

  CHECK(healthOver(argus::client::makeChannel(
            "tls:" + auth.address("127.0.0.1"))) == grpc::StatusCode::OK);
  CHECK(healthOver(argus::client::makeChannel(
            "tls:" + auth.address("localhost"))) == grpc::StatusCode::OK);
  CHECK(healthOver(argus::client::makeStreamingChannel(
            "tls:" + auth.address("127.0.0.1"))) == grpc::StatusCode::OK);
}

TEST_CASE("the client checks the name the server certificate carries")
{
  const ServingOn auth(argus::client::internalServerCredentials(
      materialOf(serviceDirectory("auth"))));
  const auto camera = argus::client::internalChannelCredentials(
      materialOf(serviceDirectory("camera")));

  CHECK(healthOver(channelNamed(auth, camera, "argus-auth")) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(channelNamed(auth, camera, "argus-identity")) ==
        grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("the server refuses a foreign client and one without a certificate "
          "and a plaintext one")
{
  const auto authMaterial = materialOf(serviceDirectory("auth"));
  const ServingOn auth(argus::client::internalServerCredentials(authMaterial));

  const auto foreign = materialOf(environment("ARGUS_INTERNAL_TLS_FOREIGN"));
  CHECK(
      healthOver(
          channelNamed(auth, argus::client::internalChannelCredentials(foreign),
                       "argus-auth")) == grpc::StatusCode::UNAVAILABLE);

  const auto anonymous =
      grpc::SslCredentials({.pem_root_certs = authMaterial.trustBundlePem,
                            .pem_private_key = "",
                            .pem_cert_chain = ""});
  CHECK(healthOver(channelNamed(auth, anonymous, "argus-auth")) ==
        grpc::StatusCode::UNAVAILABLE);

  CHECK(healthOver(argus::client::makeChannel(auth.address("127.0.0.1"))) ==
        grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("a plain target stays plaintext while a certificate is installed")
{
  argus::client::installInternalTls(materialOf(serviceDirectory("camera")));
  const ServingOn plain(grpc::InsecureServerCredentials());
  CHECK(healthOver(argus::client::makeChannel(plain.address("127.0.0.1"))) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(
            argus::client::makeChannel("tls:" + plain.address("127.0.0.1"))) ==
        grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("a second install throws")
{
  argus::client::installInternalTls(materialOf(serviceDirectory("camera")));
  CHECK_THROWS_AS(argus::client::installInternalTls(
                      materialOf(serviceDirectory("auth"))),
                  std::logic_error);
}

TEST_CASE("an install after makeChannel throws")
{
  CHECK(argus::client::makeChannel("127.0.0.1:1") != nullptr);
  CHECK_THROWS_AS(argus::client::installInternalTls(
                      materialOf(serviceDirectory("camera"))),
                  std::logic_error);
}

TEST_CASE("an install after makeStreamingChannel throws")
{
  CHECK(argus::client::makeStreamingChannel("127.0.0.1:1") != nullptr);
  CHECK_THROWS_AS(argus::client::installInternalTls(
                      materialOf(serviceDirectory("camera"))),
                  std::logic_error);
}

TEST_CASE("an empty directory setting installs nothing")
{
  CHECK_FALSE(argus::client::installInternalTlsFrom("").has_value());
  CHECK_NOTHROW(argus::client::installInternalTls(
      materialOf(serviceDirectory("camera"))));
}

TEST_CASE("a directory without the material throws and installs nothing")
{
  CHECK_THROWS_AS(static_cast<void>(argus::client::installInternalTlsFrom(
                      "/nonexistent/argus")),
                  std::runtime_error);
  CHECK_NOTHROW(argus::client::installInternalTls(
      materialOf(serviceDirectory("camera"))));
}

TEST_CASE("a service directory is installed and its material returned")
{
  const auto installed =
      argus::client::installInternalTlsFrom(serviceDirectory("camera"));
  REQUIRE(installed.has_value());
  const auto material = installed.value_or(InternalTlsMaterial{});
  CHECK(material.certificatePem ==
        materialOf(serviceDirectory("camera")).certificatePem);
  CHECK(material.trustBundlePem ==
        materialOf(serviceDirectory("camera")).trustBundlePem);

  const ServingOn auth(argus::client::internalServerCredentials(
      materialOf(serviceDirectory("auth"))));
  CHECK(healthOver(argus::client::makeChannel(
            "tls:" + auth.address("127.0.0.1"))) == grpc::StatusCode::OK);
  CHECK_THROWS_AS(static_cast<void>(argus::client::installInternalTlsFrom(
                      serviceDirectory("auth"))),
                  std::logic_error);
}

TEST_CASE("an empty install is refused and does not count as the install")
{
  CHECK_THROWS_AS(argus::client::installInternalTls(InternalTlsMaterial{}),
                  std::invalid_argument);
  auto keyless = materialOf(serviceDirectory("camera"));
  keyless.privateKeyPem.clear();
  CHECK_THROWS_AS(argus::client::installInternalTls(keyless),
                  std::invalid_argument);
  CHECK_NOTHROW(argus::client::installInternalTls(
      materialOf(serviceDirectory("camera"))));
}

TEST_CASE("material and an address give a plaintext port and a mutual TLS "
          "port")
{
  const auto ports =
      argus::client::listenPorts({.address = "127.0.0.1:0",
                                  .tlsAddress = "127.0.0.1:7243",
                                  .tls = materialOf(serviceDirectory("auth")),
                                  .services = {},
                                  .maxReceiveBytes = std::nullopt});
  REQUIRE(ports.size() == 2);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
  CHECK(ports[0].credentials != nullptr);
  CHECK(ports[1].address == "127.0.0.1:7243");
  CHECK(ports[1].mutualTls);
  CHECK(ports[1].credentials != nullptr);
  CHECK(ports[1].credentials != ports[0].credentials);
}

TEST_CASE("without material there is a plaintext port only")
{
  const auto ports =
      argus::client::listenPorts({.address = "127.0.0.1:0",
                                  .tlsAddress = "",
                                  .tls = std::nullopt,
                                  .services = {},
                                  .maxReceiveBytes = std::nullopt});
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE("an address without material is a plaintext port only")
{
  const auto ports =
      argus::client::listenPorts({.address = "127.0.0.1:0",
                                  .tlsAddress = "127.0.0.1:7243",
                                  .tls = std::nullopt,
                                  .services = {},
                                  .maxReceiveBytes = std::nullopt});
  REQUIRE(ports.size() == 1);
  CHECK(ports[0].address == "127.0.0.1:0");
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE("material without an address opens no second port")
{
  const auto ports =
      argus::client::listenPorts({.address = "127.0.0.1:0",
                                  .tlsAddress = "",
                                  .tls = materialOf(serviceDirectory("auth")),
                                  .services = {},
                                  .maxReceiveBytes = std::nullopt});
  REQUIRE(ports.size() == 1);
  CHECK_FALSE(ports[0].mutualTls);
}

TEST_CASE("no plaintext address opens nothing, mutual TLS included")
{
  const argus::client::ListenInput input{.address = "",
                                         .tlsAddress = "127.0.0.1:0",
                                         .tls = materialOf(
                                             serviceDirectory("auth")),
                                         .services = {},
                                         .maxReceiveBytes = std::nullopt};
  CHECK(argus::client::listenPorts(input).empty());
  const auto listener = argus::client::startListening(input);
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}

TEST_CASE("the mutual TLS port serves a peer of the internal CA and refuses "
          "every other")
{
  AlwaysServing health;
  auto listener = argus::client::startListening(
      {.address = "127.0.0.1:0",
       .tlsAddress = "127.0.0.1:0",
       .tls = materialOf(serviceDirectory("auth")),
       .services = {&health},
       .maxReceiveBytes = std::nullopt});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.port > 0);
  REQUIRE(listener.tlsPort > 0);
  CHECK(listener.tlsPort != listener.port);

  const auto onPort =
      [](int port,
         const std::shared_ptr<grpc::ChannelCredentials>& credentials) {
        grpc::ChannelArguments arguments;
        arguments.SetSslTargetNameOverride("argus-auth");
        return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
                                         credentials, arguments);
      };
  const auto camera = materialOf(serviceDirectory("camera"));
  CHECK(healthOver(onPort(listener.port, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(onPort(listener.tlsPort,
                          argus::client::internalChannelCredentials(camera))) ==
        grpc::StatusCode::OK);
  CHECK(healthOver(onPort(listener.tlsPort,
                          argus::client::internalChannelCredentials(materialOf(
                              environment("ARGUS_INTERNAL_TLS_FOREIGN"))))) ==
        grpc::StatusCode::UNAVAILABLE);
  CHECK(healthOver(onPort(listener.tlsPort,
                          grpc::SslCredentials(
                              {.pem_root_certs = camera.trustBundlePem,
                               .pem_private_key = "",
                               .pem_cert_chain = ""}))) ==
        grpc::StatusCode::UNAVAILABLE);
  CHECK(healthOver(
            onPort(listener.tlsPort, grpc::InsecureChannelCredentials())) ==
        grpc::StatusCode::UNAVAILABLE);
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("a mutual TLS address that cannot be bound starts nothing")
{
  AlwaysServing health;
  const auto listener = argus::client::startListening(
      {.address = "127.0.0.1:0",
       .tlsAddress = "192.0.2.1:0",
       .tls = materialOf(serviceDirectory("auth")),
       .services = {&health},
       .maxReceiveBytes = std::nullopt});
  CHECK(listener.server == nullptr);
  CHECK(listener.port == 0);
  CHECK(listener.tlsPort == 0);
}

TEST_CASE("a message over the cap is refused on both ports and one under it "
          "is served on both")
{
  AlwaysServing health;
  auto listener = argus::client::startListening(
      {.address = "127.0.0.1:0",
       .tlsAddress = "127.0.0.1:0",
       .tls = materialOf(serviceDirectory("auth")),
       .services = {&health},
       .maxReceiveBytes = 1024});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.port > 0);
  REQUIRE(listener.tlsPort > 0);

  const auto plain =
      authChannelOn(listener.port, grpc::InsecureChannelCredentials());
  const auto mutual =
      authChannelOn(listener.tlsPort,
                    argus::client::internalChannelCredentials(
                        materialOf(serviceDirectory("camera"))));
  CHECK(healthCarrying(plain, 16) == grpc::StatusCode::OK);
  CHECK(healthCarrying(mutual, 16) == grpc::StatusCode::OK);
  CHECK(healthCarrying(plain, 4096) == grpc::StatusCode::RESOURCE_EXHAUSTED);
  CHECK(healthCarrying(mutual, 4096) == grpc::StatusCode::RESOURCE_EXHAUSTED);
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("without a cap the same message is served on both ports")
{
  AlwaysServing health;
  auto listener = argus::client::startListening(
      {.address = "127.0.0.1:0",
       .tlsAddress = "127.0.0.1:0",
       .tls = materialOf(serviceDirectory("auth")),
       .services = {&health},
       .maxReceiveBytes = std::nullopt});
  REQUIRE(listener.server != nullptr);
  REQUIRE(listener.tlsPort > 0);

  CHECK(healthCarrying(authChannelOn(listener.port,
                                     grpc::InsecureChannelCredentials()),
                       4096) == grpc::StatusCode::OK);
  CHECK(
      healthCarrying(authChannelOn(listener.tlsPort,
                                   argus::client::internalChannelCredentials(
                                       materialOf(serviceDirectory("camera")))),
                     4096) == grpc::StatusCode::OK);
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("a listener that came up is no failure")
{
  AlwaysServing health;
  const argus::client::ListenInput input{.address = "127.0.0.1:0",
                                         .tlsAddress = "",
                                         .tls = std::nullopt,
                                         .services = {&health},
                                         .maxReceiveBytes = std::nullopt};
  const auto listener = argus::client::startListening(input);
  REQUIRE(listener.server != nullptr);
  CHECK_FALSE(argus::client::listenFailure({.name = "Auth RPC",
                                            .input = input,
                                            .listener = listener,
                                            .consequence = "nothing works"})
                  .has_value());
  listener.server->Shutdown(std::chrono::system_clock::now() +
                            std::chrono::seconds(2));
}

TEST_CASE("a failure names the ports that were asked for, then the "
          "consequence")
{
  const argus::client::Listener down;
  const auto material = materialOf(serviceDirectory("auth"));
  const argus::client::ListenInput both{.address = "127.0.0.1:7043",
                                        .tlsAddress = "0.0.0.0:7243",
                                        .tls = material,
                                        .services = {},
                                        .maxReceiveBytes = std::nullopt};
  const argus::client::ListenInput noMaterial{.address = "127.0.0.1:7043",
                                              .tlsAddress = "0.0.0.0:7243",
                                              .tls = std::nullopt,
                                              .services = {},
                                              .maxReceiveBytes = std::nullopt};
  const argus::client::ListenInput noTlsAddress{.address = "127.0.0.1:7043",
                                                .tlsAddress = "",
                                                .tls = material,
                                                .services = {},
                                                .maxReceiveBytes = 1024};

  CHECK(argus::client::listenFailure({.name = "Auth RPC",
                                      .input = both,
                                      .listener = down,
                                      .consequence = "without it nothing"})
            .value_or("") == "Auth RPC failed to listen on 127.0.0.1:7043 and "
                             "0.0.0.0:7243; without it nothing");
  CHECK(argus::client::listenFailure({.name = "Auth RPC",
                                      .input = noMaterial,
                                      .listener = down,
                                      .consequence = "without it nothing"})
            .value_or("") ==
        "Auth RPC failed to listen on 127.0.0.1:7043; without it nothing");
  CHECK(argus::client::listenFailure({.name = "Auth RPC",
                                      .input = noTlsAddress,
                                      .listener = down,
                                      .consequence = "without it nothing"})
            .value_or("") ==
        "Auth RPC failed to listen on 127.0.0.1:7043; without it nothing");
  CHECK(argus::client::listenFailure({.name = "gRPC server",
                                      .input = both,
                                      .listener = down,
                                      .consequence = ""})
            .value_or("") ==
        "gRPC server failed to listen on 127.0.0.1:7043 and 0.0.0.0:7243");
}
