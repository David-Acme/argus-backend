#include <doctest/doctest.h>
#include <grpc-test-support/mutual-tls-endpoint.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpc/internal-tls.hxx>
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

  grpc::ServerUnaryReactor* SensorTargets(grpc::CallbackServerContext* context,
                                          const v1::SensorTargetsRequest* request,
                                          v1::SensorTargetsResponse* out) override
  {
    {
      const std::scoped_lock lock(mutex_);
      credential_ = argus::client::metadata(context, argus::client::kCallerCredentialKey);
      asked_ = {request->camera_id()};
    }
    auto* sensor = out->add_sensors();
    sensor->set_sensor_id(4);
    sensor->set_room("1:living");
    sensor->set_pan_offset_deg(-15.0);
    sensor->set_at_ms(900);
    auto* target = sensor->add_targets();
    target->set_x_mm(500.0);
    target->set_y_mm(2000.0);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor* RoomOccupancy(grpc::CallbackServerContext* context,
                                          const v1::RoomOccupancyRequest*,
                                          v1::RoomOccupancyResponse* out) override
  {
    auto* room = out->add_rooms();
    room->set_room("1:kitchen");
    room->set_confidence(0.8);
    out->set_at_ms(1200);
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

TEST_CASE("the presence client dials the mutual TLS port with the internal certificate")
{
  ScriptedPresenceService service;
  const argus::client::test_support::MutualTlsEndpoint endpoint(
      {.service = "guard", .services = {&service}});
  REQUIRE(endpoint.tlsPort() > 0);
  argus::client::installInternalTls(
      argus::client::test_support::pkiMaterial("sync"));

  const GuardPresenceClient client(
      {.target = endpoint.tlsTarget(), .credential = kCredential});
  const auto answer = client.listPresence({7});
  REQUIRE(answer.has_value());
  CHECK(service.asked() == std::vector<int64_t>{7});

  const GuardPresenceClient plaintext(
      {.target = endpoint.plaintextToTlsPortTarget(), .credential = kCredential});
  CHECK_FALSE(plaintext.listPresence({7}).has_value());
}

TEST_CASE("a voice report without a person or a camera is not sent, and an absent guard accepts none")
{
  const GuardPresenceClient client({.target = "127.0.0.1:1", .credential = kCredential});
  CHECK_FALSE(client.reportVoice({.userId = 0, .cameraId = 3, .snrDb = 12.0F, .voiceprint = true, .atMs = 1}));
  CHECK_FALSE(client.reportVoice({.userId = 7, .cameraId = 0, .snrDb = 12.0F, .voiceprint = true, .atMs = 1}));
  CHECK_FALSE(client.reportVoice({.userId = 7, .cameraId = 3, .snrDb = 12.0F, .voiceprint = true, .atMs = 1}));
}

TEST_CASE("the presence client reads a camera's radar targets and the rooms people are in")
{
  ScriptedPresenceService service;
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  const std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  REQUIRE(server);

  const GuardPresenceClient client({.target = "127.0.0.1:" + std::to_string(port), .credential = kCredential});
  const auto sensors = client.sensorTargets(12).value_or(std::vector<v1::SensorTargets>{});
  REQUIRE(sensors.size() == 1);
  CHECK(sensors.front().sensor_id() == 4);
  CHECK(sensors.front().pan_offset_deg() == doctest::Approx(-15.0));
  REQUIRE(sensors.front().targets_size() == 1);
  CHECK(sensors.front().targets(0).y_mm() == doctest::Approx(2000.0));
  CHECK(service.asked() == std::vector<int64_t>{12});
  CHECK(service.credential() == kCredential);
  const auto rooms = client.roomOccupancy().value_or(std::vector<v1::RoomOccupancy>{});
  REQUIRE(rooms.size() == 1);
  CHECK(rooms.front().room() == "1:kitchen");
  CHECK(rooms.front().confidence() == doctest::Approx(0.8));
  CHECK_FALSE(client.sensorTargets(0).has_value());
  server->Shutdown();
}
