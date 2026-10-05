#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <argus/camera/v1/actions.grpc.pb.h>
#include <camera/camera-action-client.hxx>
#include <feature/guard/guard-assessment.hxx>
#include <llm/llm-remote.hxx>
#include <vlm/vlm-remote.hxx>

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <drogon/drogon.h>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iterator>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <utility>

#include "wait-for-boot.hxx"

using guard_test::waitForBoot;

namespace
{
class FakeCameraService final
    : public argus::camera::v1::CameraActionService::CallbackService
{
public:
  explicit FakeCameraService(std::string jpeg) : jpeg_(std::move(jpeg)) {}

  grpc::ServerUnaryReactor*
  GetPersonCrop(grpc::CallbackServerContext* context,
                const argus::camera::v1::PersonCropRequest*,
                argus::camera::v1::PersonCropResponse* response) override
  {
    response->set_jpeg(jpeg_);
    response->set_captured_at(1234);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor*
  Announce(grpc::CallbackServerContext* context,
           const argus::camera::v1::AnnounceRequest*,
           argus::camera::v1::ActionAck* response) override
  {
    response->set_accepted(true);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor*
  Alarm(grpc::CallbackServerContext* context,
        const argus::camera::v1::AlarmRequest*,
        argus::camera::v1::ActionAck* response) override
  {
    response->set_accepted(true);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor*
  SetSiren(grpc::CallbackServerContext* context,
           const argus::camera::v1::SirenRequest*,
           argus::camera::v1::ActionAck* response) override
  {
    response->set_accepted(true);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor*
  Listen(grpc::CallbackServerContext* context,
         const argus::camera::v1::ListenRequest*,
         argus::camera::v1::ListenResponse* response) override
  {
    response->set_captured(true);
    response->set_text("hola");
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

private:
  std::string jpeg_;
};

class AppRunner
{
public:
  AppRunner() : thread_([this] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!thread_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      thread_.join();
      return;
    }
    thread_.detach();
  }

  bool ready() const { return waitForBoot(std::chrono::seconds(30)); }

private:
  std::thread thread_;
};

int freePort()
{
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(probe);
  return port;
}
}

TEST_CASE("guard assessment describes the fetched crop with the live VLM" *
          doctest::skip(std::getenv("ARGUS_VLM_TEST_URL") == nullptr ||
                        std::getenv("ARGUS_VLM_TEST_IMAGE") == nullptr))
{
  const char* vlmUrl = std::getenv("ARGUS_VLM_TEST_URL");
  const char* imagePath = std::getenv("ARGUS_VLM_TEST_IMAGE");
  REQUIRE(vlmUrl != nullptr);
  REQUIRE(imagePath != nullptr);

  std::ifstream in(imagePath, std::ios::binary);
  REQUIRE(in.is_open());
  std::string jpeg((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  REQUIRE_FALSE(jpeg.empty());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(runner.ready());

  const int port = freePort();
  auto fakeCamera = std::make_unique<FakeCameraService>(jpeg);
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:" + std::to_string(port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(fakeCamera.get());
  auto server = builder.BuildAndStart();
  REQUIRE(server);

  CameraActionClient cameraClient(
      {.target = "127.0.0.1:" + std::to_string(port),
       .credential = "guard-camera"});
  VlmClient vlm(vlmUrl, 60.0);
  LlmClient llm("127.0.0.1:7032", 30000);
  GuardAssessment assessment(
      {.camera = &cameraClient, .vlm = &vlm, .llm = &llm},
      {.enabled = true,
       .mode = "agent",
       .vetoScope = "soft_only",
       .maxToolRounds = 3,
       .lang = "es"});

  const auto result = drogon::sync_wait(assessment.assess(
      {.cameraId = 6,
       .trackId = 0,
       .firstSeenMs = 0,
       .publishedAtMs = 0,
       .rule = "person_day",
       .profile = "home",
       .visitCount = 0,
       .checks = 1,
       .expectedGuest = false,
       .danger = GuardDanger::Medium,
       .personHasUser = false,
       .personName = {},
       .personRole = {},
       .personObservation = {},
       .knownTags = {},
       .greeted = false,
       .greetingText = {},
       .personReply = {},
       .replied = false,
       .replyText = {}}));
  CHECK(result.performed);
  CHECK(result.mode == "agent");
  CHECK_FALSE(result.caption.empty());
  CHECK(result.valid);
  CHECK_FALSE(result.threat.empty());

  server->Shutdown();
}
