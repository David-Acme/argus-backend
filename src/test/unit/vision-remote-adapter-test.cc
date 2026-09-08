#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-vlm-server.hxx"

#include <shared/services/config-service/config-service.hxx>
#include <shared/services/vision/remote/remote-vision-adapter.hxx>

#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdint>
#include <cstring>
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
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(10)));
  const std::string asyncCaption = drogon::sync_wait(
      adapter.describeMatAsync({.bgr = sampleImage(),
                                .prompt = "p",
                                .cameraId = "cam-1"}));
  CHECK(asyncCaption == first);
  CHECK(server.requests().at("POST /vlm/v1/describe") == 3);
  drogon::app().quit();
  runner.join();

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
