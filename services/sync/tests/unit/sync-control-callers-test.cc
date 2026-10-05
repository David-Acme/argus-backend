#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/sync-control-callers.hxx>
#include <app/rpc/sync-control-rpc-service.hxx>
#include <argus/sync/v1/sync.grpc.pb.h>
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace v1 = argus::sync::v1;

namespace
{

std::string fleetSecret()
{
  std::string value(64, 'f');
  return value;
}

std::string identitySecret()
{
  std::string value(64, 'i');
  return value;
}

std::string notificationSecret()
{
  std::string value(64, 'n');
  return value;
}

constexpr auto kResponseUpdate = static_cast<v1::SyncOperation>(10);

class Harness
{
public:
  explicit Harness(std::vector<std::pair<std::string, std::string>> callers)
      : service_(std::make_shared<const argus::client::FleetCallerGate>(
            argus::client::FleetGateConfig{
                .expectedCallers = sync_control_callers::expected(),
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
    stub_ = v1::SyncControlService::NewStub(
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
  replaceRoleRooms(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    v1::ReplaceRoleRoomsRequest request;
    request.set_user_id(7);
    request.set_old_role("not-a-role");
    request.set_new_role("guest");
    v1::ControlAck ack;
    return stub_->ReplaceRoleRooms(&context, request, &ack).error_code();
  }

  [[nodiscard]] grpc::StatusCode
  disconnectUser(const argus::client::PeerCredential& credential) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    v1::DisconnectUserRequest request;
    v1::ControlAck ack;
    return stub_->DisconnectUser(&context, request, &ack).error_code();
  }

  [[nodiscard]] grpc::StatusCode
  emit(const argus::client::PeerCredential& credential,
       v1::SyncOperation operation) const
  {
    grpc::ClientContext context;
    argus::client::setDeadline(context, 5000);
    argus::client::addPeerCredential(context, credential);
    v1::EmitToUserRequest request;
    request.mutable_frame()->set_operation(operation);
    v1::ControlAck ack;
    return stub_->EmitToUser(&context, request, &ack).error_code();
  }

private:
  SyncControlRpcService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v1::SyncControlService::Stub> stub_;
};

argus::client::PeerCredential credential(std::string value)
{
  return {.credential = std::move(value), .fleetSecret = {}};
}

argus::client::PeerCredential fleet(std::string value)
{
  return {.credential = {}, .fleetSecret = std::move(value)};
}

}

TEST_CASE("argus-notification rings and cancels calls, and nothing else")
{
  const Harness harness({{"identity", identitySecret()}, {"notification", notificationSecret()}});
  REQUIRE(harness.listening());
  const auto notification = credential(notificationSecret());

  CHECK(harness.emit(notification, v1::SYNC_OPERATION_CALL_INCOMING) ==
        grpc::StatusCode::OK);
  CHECK(harness.emit(notification, v1::SYNC_OPERATION_CALL_CANCEL) ==
        grpc::StatusCode::OK);
  CHECK(harness.emit(notification, kResponseUpdate) ==
        grpc::StatusCode::OK);
  CHECK(harness.emit(notification, v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.replaceRoleRooms(notification) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.disconnectUser(notification) ==
        grpc::StatusCode::PERMISSION_DENIED);
}

TEST_CASE("argus-identity drives the role rooms and the auth context")
{
  const Harness harness({{"identity", identitySecret()}, {"notification", notificationSecret()}});
  REQUIRE(harness.listening());
  const auto identity = credential(identitySecret());

  CHECK(harness.emit(identity, v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED) ==
        grpc::StatusCode::OK);
  CHECK(harness.replaceRoleRooms(identity) == grpc::StatusCode::OK);
  CHECK(harness.disconnectUser(identity) == grpc::StatusCode::OK);
}

TEST_CASE("a missing, wrong or retired credential is unauthenticated")
{
  const Harness harness({{"identity", identitySecret()}, {"notification", notificationSecret()}});
  REQUIRE(harness.listening());

  CHECK(harness.emit({}, v1::SYNC_OPERATION_CALL_INCOMING) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.emit(credential(std::string(64, 'x')),
                     v1::SYNC_OPERATION_CALL_INCOMING) ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.emit(fleet(fleetSecret()), v1::SYNC_OPERATION_CALL_INCOMING) ==
        grpc::StatusCode::UNAUTHENTICATED);
}

TEST_CASE("the fleet secret stands in for an unpaired notification only")
{
  const Harness harness({{"identity", identitySecret()}});
  REQUIRE(harness.listening());

  CHECK(harness.emit(fleet(fleetSecret()), v1::SYNC_OPERATION_CALL_INCOMING) ==
        grpc::StatusCode::OK);
  CHECK(harness.emit(fleet(fleetSecret()), v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.replaceRoleRooms(fleet(fleetSecret())) ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(harness.emit(credential(identitySecret()),
                     v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED) ==
        grpc::StatusCode::OK);
}

TEST_CASE("an unpaired fleet keeps working on the fleet secret alone")
{
  const Harness harness({});
  REQUIRE(harness.listening());

  CHECK(harness.replaceRoleRooms(fleet(fleetSecret())) == grpc::StatusCode::OK);
  CHECK(harness.emit(fleet(fleetSecret()), v1::SYNC_OPERATION_AUTH_CONTEXT_CHANGED) ==
        grpc::StatusCode::OK);
  CHECK(harness.disconnectUser(fleet(std::string(64, 'w'))) ==
        grpc::StatusCode::UNAUTHENTICATED);
}
