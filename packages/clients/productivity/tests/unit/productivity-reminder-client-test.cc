#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/productivity/v1/reminder.grpc.pb.h>
#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <productivity/productivity-reminder-client.hxx>

namespace
{

namespace v1 = argus::productivity::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ServedReminders final : public v1::ReminderService::CallbackService
{
public:
  Reactor* CreateReminder(Ctx* context, const v1::CreateReminderRequest*,
                          v1::ReminderResponse* out) override
  {
    out->mutable_reminder()->set_id(5);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

argus::client::CallerIdentity identityFor(int64_t userId)
{
  return {.userId = userId, .role = "resident", .device = "abc123"};
}

}

TEST_CASE("the reminder client dials the mutual TLS port with the internal certificate")
{
  ServedReminders service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "productivity", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("llm"));

  const ProductivityReminderClient client(
      {.target = endpoint.tlsTarget(), .credential = "llm-tools-secret"});
  const ReminderCreateCall call{.identity = identityFor(7),
                                .title = "Dentista",
                                .description = "",
                                .scheduledAt = 1700000000,
                                .recurrenceRule = std::nullopt,
                                .idempotencyKey = "dentista-1"};
  const auto created = client.create(call);
  CHECK(created.outcome == ReminderRpcOutcome::Success);
  REQUIRE(created.reminder.has_value());
  CHECK(created.reminder->id() == 5);

  const ProductivityReminderClient plaintext(
      {.target = endpoint.plaintextToTlsPortTarget(), .credential = "llm-tools-secret"});
  CHECK(plaintext.create(call).outcome != ReminderRpcOutcome::Success);
}
