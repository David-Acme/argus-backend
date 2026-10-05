#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/identity/v1/identity.grpc.pb.h>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <identity/identity-client.hxx>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace
{

constexpr const char* kFleetSecret = "fleet-secret";

namespace v1 = argus::identity::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ScriptedIdentityService final
    : public v1::IdentityService::CallbackService
{
public:
  v1::GetPersonResponse person;
  v1::PromotePersonResponse promote;

  std::map<std::string, std::string> seen() const
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

  Reactor* GetPerson(Ctx* context, const v1::GetPersonRequest*,
                     v1::GetPersonResponse* out) override
  {
    *out = person;
    return answer(context);
  }

  Reactor* PromotePerson(Ctx* context, const v1::PromotePersonRequest*,
                         v1::PromotePersonResponse* out) override
  {
    *out = promote;
    return answer(context);
  }

private:
  Reactor* answer(Ctx* context)
  {
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : context->client_metadata())
      headers.emplace(std::string(key.begin(), key.end()),
                      std::string(value.begin(), value.end()));
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      seen_ = std::move(headers);
    }
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::string> seen_;
};

std::unique_ptr<grpc::Server> startServer(ScriptedIdentityService& service,
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

TEST_CASE("an id the client cannot resolve never reaches the gateway")
{
  ScriptedIdentityService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const IdentityClient client(target, kFleetSecret);
  service.promote.set_promoted(true);
  service.person.mutable_person()->set_person_id(12);

  CHECK_FALSE(client.getPerson(0).has_value());
  CHECK_FALSE(client.promotePerson(
      {.personId = 0, .accessToken = "owner", .deviceHash = ""}));
  CHECK_FALSE(client.promotePerson(
      {.personId = 12, .accessToken = "", .deviceHash = ""}));

  const auto served = client.getPerson(4);
  REQUIRE(served);
  CHECK(served->personId == 12);
  CHECK(client.promotePerson(
      {.personId = 12, .accessToken = "owner", .deviceHash = ""}));

  server->Shutdown();
}

TEST_CASE("the fleet secret and the owner token are what this edge sends")
{
  ScriptedIdentityService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const IdentityClient client(target, kFleetSecret);
  service.promote.set_promoted(true);

  CHECK(client.promotePerson({.personId = 12,
                              .accessToken = "owner-token",
                              .deviceHash = "phone-hash"}));
  const auto presented = service.seen();
  CHECK(presented.at("x-argus-fleet") == kFleetSecret);
  CHECK(presented.at("authorization") == "Bearer owner-token");
  CHECK(presented.at("x-argus-device") == "phone-hash");

  CHECK(client.promotePerson(
      {.personId = 12, .accessToken = "owner-token", .deviceHash = ""}));
  CHECK(service.seen().count("x-argus-device") == 0);

  server->Shutdown();
}

TEST_CASE("a paired client presents its own credential and never the fleet "
          "secret")
{
  ScriptedIdentityService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const IdentityClient client(
      target, argus::client::PeerCredential{.credential = "guard-credential",
                                            .fleetSecret = kFleetSecret});
  service.promote.set_promoted(true);

  CHECK(client.promotePerson(
      {.personId = 12, .accessToken = "owner-token", .deviceHash = ""}));
  const auto presented = service.seen();
  CHECK(presented.at("x-argus-credential") == "guard-credential");
  CHECK(presented.count("x-argus-fleet") == 0);

  server->Shutdown();
}

TEST_CASE("the optional legs of an answer decide the profile")
{
  ScriptedIdentityService service;
  std::string target;
  auto server = startServer(service, target);
  REQUIRE(server);
  const IdentityClient client(target, kFleetSecret);

  CHECK_FALSE(client.getPerson(12).has_value());

  auto* person = service.person.mutable_person();
  person->set_person_id(12);
  const auto nameless = client.getPerson(12);
  REQUIRE(nameless);
  CHECK(nameless->personId == 12);
  CHECK_FALSE(nameless->userId.has_value());

  person->set_user_id(42);
  const auto known = client.getPerson(12);
  REQUIRE(known);
  CHECK(known->userId == 42);

  server->Shutdown();
}
