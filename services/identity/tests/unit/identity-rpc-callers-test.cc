#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-rpc-service.hxx>
#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

std::string fleetSecret()
{
  std::string value(64, 'f');
  return value;
}


std::string secretOf(const std::string& caller)
{
  return caller + std::string(48, 'k');
}

std::vector<std::pair<std::string, std::string>> everyCallerPaired()
{
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& caller : identity_callers::expected())
    pairs.emplace_back(caller, secretOf(caller));
  return pairs;
}

class Harness
{
public:
  explicit Harness(std::vector<std::pair<std::string, std::string>> pairs)
      : service_({.bus = nullptr,
                  .gate = std::make_shared<const argus::client::FleetCallerGate>(
                      argus::client::FleetGateConfig{
                          .expectedCallers = identity_callers::expected(),
                          .callerPairs = std::move(pairs),
                          .legacySecret = fleetSecret(),
                          .onFirstLegacy = {}}),
                  .auth = nullptr})
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    stub_ = argus::identity::v1::IdentityService::NewStub(
        argus::client::makeChannel("127.0.0.1:" + std::to_string(port)));
  }

  ~Harness()
  {
    if (server_)
      server_->Shutdown();
  }

  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  [[nodiscard]] bool listening() const { return server_ != nullptr; }

  [[nodiscard]] grpc::StatusCode
  registerUser(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    argus::identity::v1::RegisterUserRequest request;
    argus::identity::v1::RegisterUserResponse response;
    return stub_->RegisterUser(&context, request, &response).error_code();
  }

  [[nodiscard]] grpc::StatusCode
  tagPerson(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    argus::identity::v1::TagPersonRequest request;
    argus::identity::v1::TagPersonResponse response;
    return stub_->TagPerson(&context, request, &response).error_code();
  }

  [[nodiscard]] grpc::StatusCode
  identifyPerson(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    argus::identity::v1::IdentifyPersonRequest request;
    argus::identity::v1::IdentifyPersonResponse response;
    return stub_->IdentifyPerson(&context, request, &response).error_code();
  }

private:
  IdentityRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<argus::identity::v1::IdentityService::Stub> stub_;
};

argus::client::PeerCredential as(const std::string& caller)
{
  return {.credential = secretOf(caller), .fleetSecret = {}};
}

argus::client::PeerCredential legacy(std::string secret)
{
  return {.credential = {}, .fleetSecret = std::move(secret)};
}

}

TEST_CASE("RegisterUser answers argus-auth alone")
{
  const Harness harness(everyCallerPaired());
  REQUIRE(harness.listening());

  CHECK(harness.registerUser(as("auth")) == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(harness.registerUser(as("camera")) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.registerUser(as("voice")) ==
        grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("a missing, wrong or retired credential is unauthenticated")
{
  const Harness harness(everyCallerPaired());
  REQUIRE(harness.listening());

  CHECK(harness.registerUser({}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.registerUser({.credential = std::string(64, 'x'),
                              .fleetSecret = {}}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.registerUser(legacy(fleetSecret())) ==
        grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("person curation and the biometric oracle stay with their callers")
{
  const Harness harness(everyCallerPaired());
  REQUIRE(harness.listening());

  CHECK(harness.tagPerson(as("camera")) == grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.tagPerson(as("guard")) == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(harness.identifyPerson(as("camera")) ==
        grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(harness.identifyPerson(as("guard")) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.identifyPerson(as("llm")) ==
        grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("the fleet secret keeps an unpaired caller working, and only it")
{
  auto pairs = everyCallerPaired();
  std::erase_if(pairs, [](const auto& pair) { return pair.first == "auth"; });
  const Harness harness(std::move(pairs));
  REQUIRE(harness.listening());

  CHECK(harness.registerUser(legacy(fleetSecret())) ==
        grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(harness.tagPerson(legacy(fleetSecret())) == grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.registerUser(legacy(std::string(64, 'w'))) ==
        grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("a caller presenting both legs is judged by its own credential")
{
  const Harness harness(everyCallerPaired());
  REQUIRE(harness.listening());

  CHECK(harness.registerUser({.credential = secretOf("auth"),
                              .fleetSecret = fleetSecret()}) ==
        grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(harness.registerUser({.credential = secretOf("camera"),
                              .fleetSecret = fleetSecret()}) ==
        grpc::StatusCode::PERMISSION_DENIED);
}
