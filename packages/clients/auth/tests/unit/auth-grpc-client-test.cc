#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/auth/v1/auth.grpc.pb.h>
#include <auth/auth-client.hxx>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace
{

constexpr const char* kFleetSecret = "fleet-secret";

namespace v1 = argus::auth::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ScriptedAuthService final : public v1::AuthService::CallbackService
{
public:
  v1::ValidateTokenResponse verdict;
  bool credentialActive{false};

  std::string seenDeviceHash() const
  {
    const std::scoped_lock lock(mutex_);
    return deviceHash_;
  }

  std::map<std::string, std::string> seen() const
  {
    const std::scoped_lock lock(mutex_);
    return seen_;
  }

  Reactor* ValidateToken(Ctx* context, const v1::ValidateTokenRequest* request,
                         v1::ValidateTokenResponse* out) override
  {
    *out = verdict;
    record(context, request->has_device_hash() ? request->device_hash() : "");
    return answer(context);
  }

  Reactor* CheckDeviceCredential(Ctx* context,
                                 const v1::CheckDeviceCredentialRequest* request,
                                 v1::CheckDeviceCredentialResponse* out) override
  {
    out->set_active(credentialActive);
    record(context, request->secret_hash());
    return answer(context);
  }

private:
  Reactor* answer(Ctx* context)
  {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  void record(Ctx* context, std::string deviceHash)
  {
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : context->client_metadata())
      headers.emplace(std::string(key.begin(), key.end()),
                      std::string(value.begin(), value.end()));
    const std::scoped_lock lock(mutex_);
    seen_ = std::move(headers);
    deviceHash_ = std::move(deviceHash);
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::string> seen_;
  std::string deviceHash_;
};

std::unique_ptr<grpc::Server> startServer(ScriptedAuthService& service,
                                          std::string& target)
{
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  auto server = std::unique_ptr<grpc::Server>(builder.BuildAndStart());
  target = "127.0.0.1:" + std::to_string(port);
  return server;
}

}

TEST_CASE("the verdict and the session's user are what this edge carries back")
{
  ScriptedAuthService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const AuthClient client({.target = target, .fleetSecret = kFleetSecret});

  const auto refused = client.validateToken(
      {.accessToken = "stale", .deviceHash = "", .hasDeviceContext = false});
  if (!refused.has_value()) {
    FAIL("the client answered no verdict for a stale token");
    return;
  }
  CHECK_FALSE(refused->valid());
  CHECK(refused->reason().empty());

  service.verdict.set_valid(true);
  service.verdict.set_reason("Token expired");
  service.verdict.set_expires_at(1750000000);
  auto* user = service.verdict.mutable_user();
  user->set_user_id(42);
  user->set_name("Ana");
  user->set_last_name("Maria");
  user->set_lang("es");
  user->set_role("owner");
  user->set_is_active(true);

  const auto accepted = client.validateToken(
      {.accessToken = "live", .deviceHash = "", .hasDeviceContext = false});
  if (!accepted.has_value()) {
    FAIL("the client answered no verdict for a live token");
    return;
  }
  CHECK(accepted->valid());
  CHECK(accepted->reason() == "Token expired");
  CHECK(accepted->expires_at() == 1750000000);
  REQUIRE(accepted->has_user());
  CHECK(accepted->user().user_id() == 42);
  CHECK(accepted->user().name() == "Ana");
  CHECK(accepted->user().last_name() == "Maria");
  CHECK(accepted->user().lang() == "es");
  CHECK(accepted->user().role() == "owner");
  CHECK(accepted->user().is_active());

  server->Shutdown();
}

TEST_CASE("the fleet secret and the device leg are what this edge presents")
{
  ScriptedAuthService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const AuthClient client({.target = target, .fleetSecret = kFleetSecret});

  static_cast<void>(client.validateToken(
      {.accessToken = "live", .deviceHash = "phone-hash",
       .hasDeviceContext = true}));
  const auto headers = service.seen();
  REQUIRE(headers.contains("x-argus-fleet"));
  CHECK(headers.at("x-argus-fleet") == kFleetSecret);
  CHECK(service.seenDeviceHash() == "phone-hash");

  static_cast<void>(client.validateToken(
      {.accessToken = "live", .deviceHash = "phone-hash",
       .hasDeviceContext = false}));
  CHECK(service.seenDeviceHash().empty());

  service.credentialActive = true;
  CHECK(client.checkDeviceCredential("secret-hash") == true);
  CHECK(service.seenDeviceHash() == "secret-hash");

  service.credentialActive = false;
  CHECK(client.checkDeviceCredential("secret-hash") == false);

  server->Shutdown();
}

TEST_CASE("a listener that never answers is not an answer this edge invents")
{
  ScriptedAuthService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  static_cast<void>(server->Shutdown());
  const AuthClient client({.target = target, .fleetSecret = kFleetSecret});

  CHECK_FALSE(client
                  .validateToken({.accessToken = "live",
                                  .deviceHash = "",
                                  .hasDeviceContext = false})
                  .has_value());
  CHECK_FALSE(client.checkDeviceCredential("secret-hash").has_value());
}
