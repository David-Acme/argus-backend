#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <guard/guard-presence-client.hxx>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{

namespace v1 = argus::guard::v1;

constexpr const char* kCredential = "presence-caller-secret";

class ScriptedPresenceService final : public v1::PresenceService::CallbackService
{
public:
  grpc::ServerUnaryReactor* ListPresence(grpc::CallbackServerContext* context,
                                         const v1::ListPresenceRequest* request,
                                         v1::ListPresenceResponse* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      credential_ = argus::client::metadata(
          context, argus::client::kCallerCredentialKey);
      asked_.assign(request->user_ids().begin(), request->user_ids().end());
    }
    auto* user = out->add_users();
    user->set_user_id(7);
    user->set_overall(v1::PRESENCE_STATE_HOME);
    user->set_since(100);
    auto* environment = user->add_environments();
    environment->set_environment_id(1);
    environment->set_state(v1::PRESENCE_STATE_HOME);
    environment->set_source(v1::PRESENCE_SOURCE_LAN_SESSION);
    environment->set_since(100);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  [[nodiscard]] std::string credential() const
  {
    const std::scoped_lock lock(mutex_);
    return credential_;
  }

  [[nodiscard]] std::vector<int64_t> asked() const
  {
    const std::scoped_lock lock(mutex_);
    return asked_;
  }

private:
  mutable std::mutex mutex_;
  std::string credential_;
  std::vector<int64_t> asked_;
};

}

TEST_CASE("the presence client asks for the users it names and carries its "
          "credential")
{
  ScriptedPresenceService service;
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server);

  const GuardPresenceClient client(
      {.target = "127.0.0.1:" + std::to_string(port), .credential = kCredential});
  const auto answer = client.listPresence({7, 8});
  REQUIRE(answer.has_value());
  const auto response = answer.value_or(v1::ListPresenceResponse{});
  REQUIRE(response.users_size() == 1);
  CHECK(response.users(0).overall() == v1::PRESENCE_STATE_HOME);
  CHECK(response.users(0).environments(0).source() ==
        v1::PRESENCE_SOURCE_LAN_SESSION);
  CHECK(service.credential() == kCredential);
  CHECK(service.asked() == std::vector<int64_t>{7, 8});
  server->Shutdown();
}

TEST_CASE("a guard that does not answer is no answer")
{
  const GuardPresenceClient client(
      {.target = "127.0.0.1:1", .credential = kCredential});
  CHECK_FALSE(client.listPresence({}).has_value());
}
