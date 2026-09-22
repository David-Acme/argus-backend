#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vlm/vlm-client.hxx>

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
// Runs the app and stops it however the case body leaves. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : thread_([this] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!thread_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      thread_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
    thread_.detach();
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
