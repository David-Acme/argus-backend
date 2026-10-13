#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <grpc/grpc-client-base.hxx>
#include <grpc/health/v1/health.grpc.pb.h>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
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
