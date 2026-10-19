#include <doctest/doctest.h>
#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpc/internal-tls.hxx>
#include <guard/guard-duress-client.hxx>

#include <memory>
#include <mutex>
#include <string>

namespace
{

namespace v1 = argus::guard::v1;

constexpr const char* kCredential = "duress-caller-secret";

class ScriptedDuressService final : public v1::DuressService::CallbackService
{
public:
  grpc::ServerUnaryReactor* GetPhrase(grpc::CallbackServerContext* context,
                                      const v1::DuressPhraseRequest*,
                                      v1::DuressPhraseResponse* out) override
  {
    remember(context);
    out->set_key("phrase-key");
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor* RaiseSpoken(grpc::CallbackServerContext* context,
                                        const v1::SpokenDuressRequest* request,
                                        v1::SpokenDuressResponse* out) override
  {
    remember(context);
    {
      const std::scoped_lock lock(mutex_);
      userId_ = request->user_id();
      role_ = request->role();
      saidAt_ = request->said_at();
    }
    out->set_outcome(request->key() == "phrase-key" ? v1::SPOKEN_DURESS_OUTCOME_RAISED
                                                    : v1::SPOKEN_DURESS_OUTCOME_STALE);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  [[nodiscard]] std::string credential() const
  {
    const std::scoped_lock lock(mutex_);
    return credential_;
  }

  [[nodiscard]] int64_t userId() const
  {
    const std::scoped_lock lock(mutex_);
    return userId_;
  }

  [[nodiscard]] std::string role() const
  {
    const std::scoped_lock lock(mutex_);
    return role_;
  }

  [[nodiscard]] int64_t saidAt() const
  {
    const std::scoped_lock lock(mutex_);
    return saidAt_;
  }

private:
  void remember(const grpc::CallbackServerContext* context)
  {
    const std::scoped_lock lock(mutex_);
    credential_ = argus::client::metadata(context, argus::client::kCallerCredentialKey);
  }

  mutable std::mutex mutex_;
  std::string credential_;
  int64_t userId_{0};
  std::string role_;
  int64_t saidAt_{0};
};

struct ScriptedServer
{
  ScriptedServer()
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~ScriptedServer()
  {
    if (server)
      server->Shutdown();
  }

  ScriptedServer(const ScriptedServer&) = delete;
  ScriptedServer& operator=(const ScriptedServer&) = delete;

  [[nodiscard]] std::string target() const { return "127.0.0.1:" + std::to_string(port); }

  ScriptedDuressService service;
  int port{0};
  std::unique_ptr<grpc::Server> server;
};

}

TEST_CASE("the duress client reads the phrase key with its credential")
{
  ScriptedServer scripted;
  REQUIRE(scripted.server);
  const GuardDuressClient client({.target = scripted.target(), .credential = kCredential});
  CHECK(client.phraseKey() == std::optional<std::string>("phrase-key"));
  CHECK(scripted.service.credential() == kCredential);
}

TEST_CASE("the duress client raises a spoken alert for the speaker it names")
{
  ScriptedServer scripted;
  REQUIRE(scripted.server);
  const GuardDuressClient client({.target = scripted.target(), .credential = kCredential});
  CHECK(client.raiseSpoken({.userId = 9, .role = "resident", .key = "phrase-key", .saidAt = 1'790'000'000}) ==
        SpokenDuressReply::Raised);
  CHECK(scripted.service.userId() == 9);
  CHECK(scripted.service.role() == "resident");
  CHECK(scripted.service.saidAt() == 1'790'000'000);
  CHECK(client.raiseSpoken({.userId = 9, .role = "resident", .key = "old-key", .saidAt = 1'790'000'000}) ==
        SpokenDuressReply::Stale);
}

TEST_CASE("a raise that names no time reaches guard with none")
{
  ScriptedServer scripted;
  REQUIRE(scripted.server);
  const GuardDuressClient client({.target = scripted.target(), .credential = kCredential});
  CHECK(client.raiseSpoken({.userId = 9, .role = "resident", .key = "phrase-key", .saidAt = 0}) ==
        SpokenDuressReply::Raised);
  CHECK(scripted.service.saidAt() == 0);
}

TEST_CASE("a guard that does not answer is no key and an unavailable raise")
{
  const GuardDuressClient client({.target = "127.0.0.1:1", .credential = kCredential});
  CHECK_FALSE(client.phraseKey().has_value());
  CHECK(client.raiseSpoken({.userId = 9, .role = "owner", .key = "phrase-key", .saidAt = 0}) ==
        SpokenDuressReply::Unavailable);
}

TEST_CASE("the duress client dials the mutual TLS port with the internal certificate")
{
  ScriptedDuressService service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "guard", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("llm"));

  const GuardDuressClient client({.target = endpoint.tlsTarget(), .credential = kCredential});
  const auto key = client.phraseKey();
  if (!key) {
    FAIL("the phrase key is empty");
    return;
  }
  CHECK(*key == "phrase-key");
  CHECK(client.raiseSpoken({.userId = 9, .role = "resident", .key = "phrase-key", .saidAt = 0}) ==
        SpokenDuressReply::Raised);

  const GuardDuressClient plaintext({.target = endpoint.plaintextToTlsPortTarget(), .credential = kCredential});
  CHECK_FALSE(plaintext.phraseKey().has_value());
  CHECK(plaintext.raiseSpoken({.userId = 9, .role = "resident", .key = "phrase-key", .saidAt = 0}) ==
        SpokenDuressReply::Unavailable);
}
