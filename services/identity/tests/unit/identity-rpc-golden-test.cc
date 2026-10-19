#include <doctest/doctest.h>

#include <app/rpc/identity-rpc-service.hxx>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <identity-rpc-golden.hxx>
#include <json/json.h>
#include <memory>
#include <string>

namespace
{

constexpr int kInlineMaxReceiveBytes = 12 * 1024 * 1024;

class InlineIdentityRpc
{
public:
  explicit InlineIdentityRpc(IdentityRpcService& service)
  {
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kInlineMaxReceiveBytes);
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(&service);
    server_ = builder.BuildAndStart();
  }

  ~InlineIdentityRpc()
  {
    if (server_)
      server_->Shutdown(std::chrono::system_clock::now() +
                        std::chrono::seconds(2));
  }

  InlineIdentityRpc(const InlineIdentityRpc&) = delete;
  InlineIdentityRpc& operator=(const InlineIdentityRpc&) = delete;
  InlineIdentityRpc(InlineIdentityRpc&&) = delete;
  InlineIdentityRpc& operator=(InlineIdentityRpc&&) = delete;

  [[nodiscard]] bool listening() const { return server_ != nullptr; }

  [[nodiscard]] std::string target() const
  {
    return "127.0.0.1:" + std::to_string(port_);
  }

private:
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
};

}

TEST_CASE("the inline identity RPC answers the recorded golden")
{
  const IdentityGoldenApp app;
  IdentityRpcService service(
      {.bus = nullptr, .gate = goldenGate(), .auth = nullptr});
  const InlineIdentityRpc rpc(service);
  REQUIRE(rpc.listening());

  const auto answers = askIdentityGolden(rpc.target());
  REQUIRE(answers.size() == 11);
  if (recordingIdentityGolden()) {
    recordIdentityGolden(answers);
    MESSAGE("recorded the golden identity answers");
    return;
  }
  const Json::Value golden = identityGoldenFile();
  CHECK(golden.size() == answers.size());
  for (const auto& answer : answers) {
    INFO(answer.name);
    REQUIRE(golden.isMember(answer.name));
    CHECK(golden[answer.name] == answer.answer);
  }
}
