#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <argus/camera/v1/actions.grpc.pb.h>
#include <camera/camera-action-client.hxx>
#include <doctest/doctest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// Pins the client's own reading of the action wire: which inputs never leave
// the process, what an ack becomes, and how a transport failure is read.
namespace
{

namespace v1 = argus::camera::v1;

using Ctx = grpc::CallbackServerContext;
using Reactor = grpc::ServerUnaryReactor;

// Answers the announced command with whatever the case put in it.
class ScriptedActionService final
    : public v1::CameraActionService::CallbackService
{
public:
  grpc::Status status = grpc::Status::OK;
  v1::ActionAck ack;

  Reactor* Announce(Ctx* context, const v1::AnnounceRequest*,
                    v1::ActionAck* out) override
  {
    *out = ack;
    auto* reactor = context->DefaultReactor();
    reactor->Finish(status);
    return reactor;
  }
};

std::unique_ptr<grpc::Server> startServer(ScriptedActionService& service,
                                          int& port)
{
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &port);
  builder.RegisterService(&service);
  return std::unique_ptr<grpc::Server>(builder.BuildAndStart());
}

std::string loopback(int port)
{
  return "127.0.0.1:" + std::to_string(port);
}

} // namespace

TEST_CASE("a bad camera id is refused locally, an unreachable one is not")
{
  // The dead target is the control: the same call with a valid id comes back
  // UNAVAILABLE, so INVALID_ARGUMENT proves the client never dialled out.
  const CameraActionClient client({.target = "127.0.0.1:1", .credential = ""});

  const auto refused =
      client.announce({.cameraId = 0, .text = "Hola", .commandId = ""});
  CHECK(refused.status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(refused.status.error_message() == "camera id is required");
  CHECK(refused.outcome == CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED);
  CHECK_FALSE(refused.retryable());

  CHECK(client.alarm({.cameraId = -1, .seconds = 5, .commandId = ""})
            .status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(client.setSiren({.cameraId = 0, .enabled = true, .commandId = ""})
            .status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(
      client.listen({.cameraId = 3, .commandId = ""}).status.error_message() ==
      "camera id and command id are required");
  CHECK_FALSE(client.personCrop({.cameraId = 0, .trackId = 7}).has_value());

  const auto down =
      client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
  CHECK(down.status.error_code() == grpc::StatusCode::UNAVAILABLE);
  CHECK(down.outcome == CameraCommandOutcome::RETRYABLE_FAILED);
  CHECK(down.retryable());
}

TEST_CASE("an ack's outcome is read off the wire, never inferred")
{
  ScriptedActionService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  const CameraActionClient client({.target = loopback(port), .credential = ""});

  SUBCASE("an explicit outcome outranks accepted and duplicate")
  {
    service.ack.set_accepted(true);
    service.ack.set_duplicate(true);
    service.ack.set_outcome(CameraCommandOutcome::INDETERMINATE);
    const auto result =
        client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
    CHECK(result.outcome == CameraCommandOutcome::INDETERMINATE);
    CHECK_FALSE(result.succeeded());
    CHECK(result.duplicate);
  }

  SUBCASE("duplicate and accepted decide first, then the detail alone")
  {
    service.ack.set_duplicate(true);
    const auto repeated =
        client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
    CHECK(repeated.outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED);
    service.ack.clear_duplicate();
    service.ack.set_accepted(true);
    service.ack.set_detail("spoken");
    const auto accepted =
        client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
    CHECK(accepted.outcome == CameraCommandOutcome::SUCCEEDED);
    CHECK(accepted.detail == "spoken");
    // The detail fallback is for an ack carrying no outcome and no accepted
    // flag: the service derives `accepted` from an outcome it always sets
    // (`finishAck`, camera-action-rpc-service.cc:71-80), so "in_flight" never
    // rides with accepted, and accepted outranks detail in any case.
    service.ack.clear_accepted();
    service.ack.set_detail("in_flight");
    CHECK(client.announce({.cameraId = 4, .text = "Hola", .commandId = ""})
              .inFlight());
    service.ack.set_detail("command_id_conflict");
    const auto conflicted =
        client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
    CHECK(conflicted.outcome == CameraCommandOutcome::CONFLICT);
  }

  SUBCASE("an ack that says nothing is a rejection, not a retry")
  {
    const auto silent =
        client.announce({.cameraId = 4, .text = "Hola", .commandId = ""});
    CHECK(silent.transportOk());
    CHECK(silent.outcome == CameraCommandOutcome::REJECTED);
    CHECK_FALSE(silent.retryable());
  }

  server->Shutdown();
}

TEST_CASE("a transport failure is classified by whether the command ran")
{
  ScriptedActionService service;
  int port = 0;
  auto server = startServer(service, port);
  REQUIRE(server);
  const CameraActionClient client({.target = loopback(port), .credential = ""});

  service.status = grpc::Status(grpc::StatusCode::NOT_FOUND, "no camera");
  const auto refused =
      client.announce({.cameraId = 99, .text = "Hola", .commandId = ""});
  CHECK(refused.outcome == CameraCommandOutcome::REJECTED);
  CHECK_FALSE(refused.retryable());

  service.status = grpc::Status(grpc::StatusCode::INTERNAL, "boom");
  const auto ambiguous =
      client.announce({.cameraId = 99, .text = "Hola", .commandId = ""});
  CHECK(ambiguous.outcome == CameraCommandOutcome::INDETERMINATE);
  CHECK(ambiguous.indeterminate());
  CHECK_FALSE(ambiguous.rejected());

  server->Shutdown();
}
