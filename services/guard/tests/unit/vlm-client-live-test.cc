#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vlm/vlm-remote.hxx>

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
}

TEST_CASE("the VLM client describes a real JPEG against a live service" *
          doctest::skip(std::getenv("ARGUS_VLM_TEST_URL") == nullptr ||
                        std::getenv("ARGUS_VLM_TEST_IMAGE") == nullptr))
{
  const char* url = std::getenv("ARGUS_VLM_TEST_URL");
  const char* imagePath = std::getenv("ARGUS_VLM_TEST_IMAGE");
  REQUIRE(url != nullptr);
  REQUIRE(imagePath != nullptr);

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
