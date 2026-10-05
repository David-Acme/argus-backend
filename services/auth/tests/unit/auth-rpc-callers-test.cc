#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/auth-callers.hxx>
#include <app/rpc/auth-rpc-service.hxx>
#include <argus/auth/v1/auth.grpc.pb.h>
#include <config/auth-config.hxx>
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


std::vector<std::pair<std::string, std::string>> everyCallerPaired()
{
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const auto& caller : auth_callers::expected())
    pairs.emplace_back(caller, caller + std::string(48, 'k'));
  return pairs;
}

class Harness
{
public:
  explicit Harness(std::vector<std::pair<std::string, std::string>> callers)
      : service_({.sessions = nullptr, .deviceCredentials = nullptr},
                 std::make_shared<const argus::client::FleetCallerGate>(
                     argus::client::FleetGateConfig{
                         .expectedCallers = auth_callers::expected(),
                         .callerPairs = std::move(callers),
                         .legacySecret = fleetSecret(),
                         .onFirstLegacy = {}}))
  {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    stub_ = argus::auth::v1::AuthService::NewStub(
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
  validate(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    argus::auth::v1::ValidateTokenRequest request;
    argus::auth::v1::ValidateTokenResponse response;
    return stub_->ValidateToken(&context, request, &response).error_code();
  }

  [[nodiscard]] grpc::StatusCode
  checkDevice(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    argus::auth::v1::CheckDeviceCredentialRequest request;
    argus::auth::v1::CheckDeviceCredentialResponse response;
    return stub_->CheckDeviceCredential(&context, request, &response)
        .error_code();
  }

private:
  AuthRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<argus::auth::v1::AuthService::Stub> stub_;
};

}

TEST_CASE("the session verdict refuses a missing, wrong or retired credential")
{
  const Harness harness(everyCallerPaired());
  REQUIRE(harness.listening());

  CHECK(harness.validate({}) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.validate({.credential = std::string(64, 'x'), .fleetSecret = {}}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.validate({.credential = {}, .fleetSecret = fleetSecret()}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.checkDevice({.credential = {}, .fleetSecret = fleetSecret()}) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.checkDevice({.credential = "CHANGE_ME_CAMERA_AUTH",
                             .fleetSecret = {}}) ==
        grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("the rpc config refuses a short credential and an unpaired exposed "
          "listener, and accepts a paired one without a fleet secret")
{
  AuthRpcConfig config;
  config.listener.host = "0.0.0.0";
  CHECK(config.secretProblem().has_value());

  config.callers = {{"camera", "CHANGE_ME_CAMERA_AUTH"}};
  CHECK(config.secretProblem().has_value());

  config.callers = {{"camera", std::string(64, 'c')}};
  CHECK_FALSE(config.secretProblem().has_value());

  config.callers = {{"camera", "short"}};
  CHECK(config.secretProblem().has_value());

  config.callers = {};
  config.secret = fleetSecret();
  CHECK_FALSE(config.secretProblem().has_value());
}
