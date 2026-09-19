#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/vision/remote/vlm-client.hxx>

#include <chrono>
#include <cstdlib>
#include <drogon/drogon.h>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "wait-for-boot.hxx"

using guard_test::waitForBoot;

namespace
{
// Quits the app and joins even when an assertion aborts the test case.
class AppRunner
{
public:
  AppRunner() : thread_([this] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    drogon::app().quit();
    if (thread_.joinable())
      thread_.join();
  }

  bool ready() const { return waitForBoot(std::chrono::seconds(30)); }

private:
  std::thread thread_;
};
} // namespace

// Opt-in live check: set ARGUS_VLM_TEST_URL and ARGUS_VLM_TEST_IMAGE to run
// against a real argus-vlm instance; silently passes otherwise.
TEST_CASE("the VLM client describes a real JPEG against a live service")
{
  const char* url = std::getenv("ARGUS_VLM_TEST_URL");
  const char* imagePath = std::getenv("ARGUS_VLM_TEST_IMAGE");
  if (!url || !imagePath)
    return;

  std::ifstream in(imagePath, std::ios::binary);
  REQUIRE(in.is_open());
  const std::string jpeg((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  REQUIRE_FALSE(jpeg.empty());

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(runner.ready());

  VlmClient client(url, 60.0);
  const auto result = drogon::sync_wait(client.describe(
      {.jpeg = jpeg,
       .prompt = "Describe this image in one short sentence.",
       .cameraId = "guard-live-test"}));
  REQUIRE_MESSAGE(result, "argus-vlm did not answer with a caption");
  CHECK_FALSE(result->caption.empty());
}
