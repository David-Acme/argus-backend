#include <argus/identity/v1/sync.grpc.pb.h>
#include <doctest/doctest.h>

#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <identity/identity-sync-client.hxx>

namespace
{

constexpr const char* kFleetSecret = "fleet-secret";

namespace v1 = argus::identity::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

class ServedUsers final : public v1::SyncService::CallbackService
{
public:
  Reactor* PullTable(Ctx* context, const v1::PullTableRequest*,
                     v1::PullTableResponse* out) override
  {
    out->mutable_user()->add_created()->set_id(9);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};

}

TEST_CASE("the identity sync client dials the mutual TLS port with the internal certificate")
{
  ServedUsers service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "identity", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("sync"));

  v1::PullTableRequest request;
  request.mutable_user()->set_required_create(true);
  const argus::client::CallerIdentity identity{
      .userId = 7, .role = "owner", .device = "abc123"};

  const IdentitySyncClient client(
      {.target = endpoint.tlsTarget(), .fleetSecret = kFleetSecret});
  const auto rows = client.pullTable(request, identity);
  if (!rows) {
    FAIL("the sync pull returned no rows");
    return;
  }
  CHECK(rows->user().created(0).id() == 9);

  const IdentitySyncClient plaintext(
      {.target = endpoint.plaintextToTlsPortTarget(), .fleetSecret = kFleetSecret});
  CHECK_FALSE(plaintext.pullTable(request, identity).has_value());
}
