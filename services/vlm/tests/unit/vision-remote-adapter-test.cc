#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-vlm-server.hxx"

#include <config/config-service.hxx>
#include <shared/services/vision/remote/remote-vision-adapter.hxx>

#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

// Independent reference implementation of the cache-key FNV-1 hash contract.
uint64_t referenceHash(const unsigned char* data, size_t len)
{
  uint64_t h = 14695981039346656037ULL;
  const size_t blocks = len / 8;
  for (size_t b = 0; b < blocks; ++b) {
    uint64_t word = 0;
    std::memcpy(&word, data + b * 8, sizeof(word));
    h ^= word;
    h *= 1099511628211ULL;
  }
  for (size_t i = blocks * 8; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

uint64_t referenceKey(const std::string& jpeg, const std::string& prompt)
{
  uint64_t h = referenceHash(
      reinterpret_cast<const unsigned char*>(jpeg.data()), jpeg.size());
  h ^= referenceHash(
      reinterpret_cast<const unsigned char*>(prompt.data()), prompt.size());
  h *= 1099511628211ULL;
  return h;
}

cv::Mat sampleImage()
{
  cv::Mat img(64, 96, CV_8UC3, cv::Scalar(30, 40, 55));
  cv::rectangle(img, cv::Rect(8, 10, 24, 30), cv::Scalar(240, 240, 240), -1);
  cv::circle(img, cv::Point(70, 32), 10, cv::Scalar(60, 90, 220), -1);
  return img;
}

std::string jpegOf(const cv::Mat& img)
{
  std::vector<unsigned char> jpeg;
  cv::imencode(".jpg", img, jpeg, {cv::IMWRITE_JPEG_QUALITY, 90});
  return std::string(jpeg.begin(), jpeg.end());
}

// Runs the app and stops it however the case body leaves. A joinable
// std::thread destroyed by unwinding calls std::terminate, which reports an
// ordinary statement failure as a SIGABRT with no assertion behind it.
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
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
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

void pointAt(const std::string& url)
{
  ConfigService::setRuntimeString("vision.remote_url", url);
}

constexpr const char* kScratchConfig = "vision-remote-adapter-test.toml";

} // namespace

TEST_CASE("the remote vision adapter serves describeMat over the argus-vlm wire")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[vision]\nremote_url = \"\"\n";
  }
  ConfigService::load(kScratchConfig);

  const std::string jpeg = jpegOf(sampleImage());

  CHECK(RemoteVisionServiceAdapter::cacheKey(jpeg, "p") ==
        RemoteVisionServiceAdapter::cacheKey(jpeg, "p"));
  CHECK(RemoteVisionServiceAdapter::cacheKey(jpeg, "p") !=
        RemoteVisionServiceAdapter::cacheKey(jpeg, "other"));
  CHECK(RemoteVisionServiceAdapter::cacheKey(jpeg, "p") ==
        referenceKey(jpeg, "p"));

  pointAt("");
  RemoteVisionServiceAdapter disabled;
  CHECK_FALSE(disabled.initialize());
  CHECK_FALSE(disabled.isLoaded());
  CHECK(disabled.describeMat({.bgr = sampleImage(),
                              .prompt = "p",
                              .cameraId = "cam-1"}).empty());

  FakeVlmServer server;
  pointAt("http://127.0.0.1:" + std::to_string(server.port()));
  RemoteVisionServiceAdapter adapter;
  REQUIRE(adapter.initialize());
  CHECK(adapter.isLoaded());

  const std::string first = adapter.describeMat({.bgr = sampleImage(),
                                                 .prompt = "p",
                                                 .cameraId = "cam-1"});
  CHECK(first == "canned caption");
  const Json::Value body = server.lastBody();
  CHECK(body.isMember("image_b64"));
  const std::string decoded =
      drogon::utils::base64Decode(body["image_b64"].asString());
  CHECK(decoded.size() > 2);
  CHECK(static_cast<unsigned char>(decoded[0]) == 0xFF);
  CHECK(static_cast<unsigned char>(decoded[1]) == 0xD8);
  CHECK(body["prompt"] == "p");
  CHECK(body["camera_id"] == "cam-1");
  CHECK(server.requests().at("POST /vlm/v1/describe") == 1);

  const std::string cached = adapter.describeMat({.bgr = sampleImage(),
                                                  .prompt = "p",
                                                  .cameraId = "cam-1"});
  CHECK(cached == first);
  CHECK(server.requests().at("POST /vlm/v1/describe") == 1);

  const std::string bare = adapter.describeMat(
      {.bgr = sampleImage(), .prompt = "", .cameraId = ""});
  CHECK(bare == "canned caption");
  CHECK_FALSE(server.lastBody().isMember("prompt"));
  CHECK_FALSE(server.lastBody().isMember("camera_id"));
  CHECK(server.requests().at("POST /vlm/v1/describe") == 2);

  CHECK(adapter.describeMat({.bgr = sampleImage(),
                             .prompt = "other",
                             .cameraId = "cam-1"}) == "canned caption");
  CHECK(server.requests().at("POST /vlm/v1/describe") == 3);

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  std::optional<AppRunner> runner;
  runner.emplace();
  REQUIRE(waitForBoot(std::chrono::seconds(10)));
  const std::string asyncCaption = drogon::sync_wait(
      adapter.describeMatAsync({.bgr = sampleImage(),
                                .prompt = "p",
                                .cameraId = "cam-1"}));
  CHECK(asyncCaption == first);
  CHECK(server.requests().at("POST /vlm/v1/describe") == 3);
  // The owner quits and joins the app; releasing it here stops the app exactly
  // where the explicit quit/join stood, on every path out of the case.
  runner.reset();

  FakeVlmServer downServer(503);
  pointAt("http://127.0.0.1:" + std::to_string(downServer.port()));
  RemoteVisionServiceAdapter downAdapter;
  REQUIRE(downAdapter.initialize());
  bool caught = false;
  try {
    (void)downAdapter.describeMat({.bgr = sampleImage(),
                                   .prompt = "p",
                                   .cameraId = "cam-1"});
  }
  catch (const std::exception& e) {
    caught = true;
    CHECK(std::string(e.what()).find("VLM_NOT_LOADED") != std::string::npos);
  }
  CHECK(caught);
  downAdapter.shutdown();

  pointAt("http://127.0.0.1:1");
  RemoteVisionServiceAdapter deadAdapter;
  REQUIRE(deadAdapter.initialize());
  caught = false;
  try {
    (void)deadAdapter.describeMat({.bgr = sampleImage(),
                                   .prompt = "p",
                                   .cameraId = "cam-1"});
  }
  catch (const std::exception& e) {
    caught = true;
    CHECK(std::string(e.what()).find("unreachable") != std::string::npos);
  }
  CHECK(caught);

  pointAt("");
  std::remove(kScratchConfig);
}
