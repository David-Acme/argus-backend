#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/vlm-controller.hxx>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>

#include <llama.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json/reader.h>
#include <json/value.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{

#ifndef ARGUS_TEST_VLM_MODELS_DIR
#define ARGUS_TEST_VLM_MODELS_DIR "models/vision/lfm2vl-25"
#endif

constexpr const char* kScratchConfig = "vlm-wire-test.toml";

struct HttpReply
{
  int status{0};
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string toLower(std::string value)
{
  for (auto& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

HttpReply request(int port, const std::string& method,
                  const std::string& path, const std::string& body,
                  const std::string& contentType = "")
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
          0);

  std::string wire = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
  if (!body.empty() || !contentType.empty()) {
    wire += "Content-Type: " +
            (contentType.empty() ? std::string("application/octet-stream")
                                 : contentType) +
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  wire += "Connection: close\r\n\r\n" + body;

  size_t sent = 0;
  while (sent < wire.size()) {
    const auto n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
    REQUIRE(n > 0);
    sent += static_cast<size_t>(n);
  }

  std::string data;
  char buffer[65536];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0)
      break;
    data.append(buffer, static_cast<size_t>(n));
  }
  ::close(fd);

  const auto split = data.find("\r\n\r\n");
  REQUIRE(split != std::string::npos);
  const std::string head = data.substr(0, split);

  const auto eol = head.find("\r\n");
  const std::string statusLine = head.substr(0, eol);
  const auto space1 = statusLine.find(' ');
  const auto space2 = statusLine.find(' ', space1 + 1);
  const int status =
      std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));

  std::map<std::string, std::string> headers;
  std::string::size_type cursor = eol + 2;
  while (cursor < head.size()) {
    const auto lineEnd = head.find("\r\n", cursor);
    if (lineEnd == std::string::npos)
      break;
    const auto colon = head.find(':', cursor);
    if (colon != std::string::npos)
      headers[toLower(head.substr(cursor, colon - cursor))] =
          head.substr(colon + 2, lineEnd - colon - 2);
    cursor = lineEnd + 2;
  }

  HttpReply reply;
  reply.status = status;
  reply.headers = std::move(headers);
  reply.body = data.substr(split + 4);
  return reply;
}

Json::Value envelope(const HttpReply& reply)
{
  Json::Value json;
  Json::Reader reader;
  REQUIRE(reader.parse(reply.body, json));
  return json;
}

std::string postDescribe(int port, const std::string& imageB64,
                         const std::string& prompt,
                         const std::string& cameraId = {})
{
  Json::Value body(Json::objectValue);
  body["image_b64"] = imageB64;
  if (!prompt.empty())
    body["prompt"] = prompt;
  if (!cameraId.empty())
    body["camera_id"] = cameraId;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return request(port, "POST", "/vlm/v1/describe",
                 Json::writeString(builder, body), "application/json")
      .body;
}

// The exact synthetic bench frame labs/vlm-bench builds.
cv::Mat syntheticImage(int w, int h)
{
  cv::Mat img(h, w, CV_8UC3, cv::Scalar(30, 40, 55));
  for (int y = 0; y < h; ++y) {
    auto* row = img.ptr<unsigned char>(y);
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = static_cast<unsigned char>((x * 255) / std::max(1, w));
      row[x * 3 + 1] = static_cast<unsigned char>((y * 255) / std::max(1, h));
      row[x * 3 + 2] =
          static_cast<unsigned char>(((x + y) * 127) / std::max(1, w + h));
    }
  }
  cv::rectangle(img, cv::Rect(w / 6, h / 5, w / 4, h / 2),
                cv::Scalar(240, 240, 240), -1);
  cv::circle(img, cv::Point(w * 2 / 3, h / 2), std::min(w, h) / 7,
             cv::Scalar(60, 90, 220), -1);
  cv::putText(img, "ARGUS", cv::Point(w / 8, h * 9 / 10),
              cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(255, 255, 255), 3);
  return img;
}

std::string jpegB64(const cv::Mat& img)
{
  std::vector<unsigned char> jpeg;
  cv::imencode(".jpg", img, jpeg, {cv::IMWRITE_JPEG_QUALITY, 90});
  return drogon::utils::base64Encode(jpeg.data(), jpeg.size());
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

} // namespace

TEST_CASE("the argus-vlm internal wire serves the vision capacity")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[vision]\n"
           << "model_path = \"" << ARGUS_TEST_VLM_MODELS_DIR
           << "/lm-Q8_0.gguf\"\n"
           << "mmproj_path = \"" << ARGUS_TEST_VLM_MODELS_DIR
           << "/mmproj-F16.gguf\"\n"
           << "max_tokens = 64\n"
              "[drogon.app]\nnumber_of_threads = 2\n";
  }
  ConfigService::load(kScratchConfig);

  llama_backend_init();

  const auto vlm = std::make_shared<VlmController>();
  vlm->initEngine();
  REQUIRE_MESSAGE(vlm->isEngineLoaded(),
                  "Vision engine failed to load from " ARGUS_TEST_VLM_MODELS_DIR
                  " — run scripts/setup.sh first");

  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().setClientMaxBodySize(64 * 1024 * 1024);
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-vlm"}));
  drogon::app().registerController(vlm);
  drogon::app().setExceptionHandler(AppConfig::handleException);
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });
  drogon::app().addListener("127.0.0.1", 0);

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  const auto listeners = drogon::app().getListeners();
  REQUIRE_FALSE(listeners.empty());
  const int port = listeners.front().toPort();
  REQUIRE(port > 0);

  const auto health = request(port, "GET", "/health", "");
  CHECK(health.status == 200);
  CHECK(envelope(health)["info"]["service"] == "argus-vlm");

  const auto config = request(port, "GET", "/vlm/v1/config", "");
  const Json::Value configJson = envelope(config);
  CHECK(config.status == 200);
  CHECK(configJson["info"]["loaded"].asBool());
  CHECK(configJson["info"]["maxInputPx"].asInt() == 384);
  CHECK(configJson["info"]["defaultMaxTokens"].asInt() == 64);

  const cv::Mat img = syntheticImage(640, 480);
  const std::string imageB64 = jpegB64(img);
  const std::string personPrompt =
      "Is there a person in this image? Answer yes or no.";

  const auto t0 = std::chrono::steady_clock::now();
  const std::string wireBody =
      postDescribe(port, imageB64, personPrompt, "cam-1");
  const auto wireMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t0)
          .count();
  Json::Value wireJson;
  Json::Reader reader;
  REQUIRE(reader.parse(wireBody, wireJson));
  CHECK(wireJson["status"].asInt() == 200);
  CHECK(wireJson["info"].isMember("caption"));
  const std::string wireCaption = wireJson["info"]["caption"].asString();
  CHECK_FALSE(wireCaption.empty());
  MESSAGE("wire caption: \"", wireCaption, "\" (", static_cast<int>(wireMs),
          " ms)");

  const std::string jpegBytes = drogon::utils::base64Decode(imageB64);
  const cv::Mat encoded(1, static_cast<int>(jpegBytes.size()), CV_8UC1,
                        const_cast<char*>(jpegBytes.data()));
  const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
  REQUIRE_FALSE(decoded.empty());
  const std::string directCaption =
      vlm->service().describeMat(decoded, personPrompt, 0);
  MESSAGE("direct caption: \"", directCaption, "\"");
  CHECK(directCaption == wireCaption);

  const std::string colorsPrompt = "Describe the dominant colors in this image.";
  const std::string engineCaption =
      vlm->service().describeMat(decoded, colorsPrompt, 0);
  MESSAGE("engine caption (cache-busted): \"", engineCaption, "\"");
  CHECK_FALSE(engineCaption.empty());

  const auto t1 = std::chrono::steady_clock::now();
  const std::string cachedBody = postDescribe(port, imageB64, personPrompt, "cam-1");
  const auto cachedMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - t1)
          .count();
  Json::Value cachedJson;
  REQUIRE(reader.parse(cachedBody, cachedJson));
  CHECK(cachedJson["info"]["caption"].asString() == wireCaption);
  MESSAGE("cached repeat: ", static_cast<int>(cachedMs), " ms");

  const std::string defaultBody = postDescribe(port, imageB64, "");
  Json::Value defaultJson;
  REQUIRE(reader.parse(defaultBody, defaultJson));
  CHECK(defaultJson["status"].asInt() == 200);
  const std::string defaultCaption = defaultJson["info"]["caption"].asString();
  MESSAGE("default-prompt caption: \"", defaultCaption, "\"");
  CHECK_FALSE(defaultCaption.empty());

  const auto notJson =
      request(port, "POST", "/vlm/v1/describe", "not json", "text/plain");
  CHECK(notJson.status == 400);
  CHECK(envelope(notJson)["errors"]["code"] == "BAD_REQUEST");

  const std::string missingImage =
      postDescribe(port, "", personPrompt);
  Json::Value missingJson;
  REQUIRE(reader.parse(missingImage, missingJson));
  CHECK(missingJson["status"].asInt() == 422);
  CHECK(missingJson["errors"]["code"] == "VALIDATION_ERROR");
  CHECK(missingJson["errors"]["fields"].isMember("imageB64"));

  const std::string badB64 =
      postDescribe(port, "not base64 !!!", personPrompt);
  Json::Value badB64Json;
  REQUIRE(reader.parse(badB64, badB64Json));
  CHECK(badB64Json["status"].asInt() == 422);
  CHECK(badB64Json["errors"]["fields"].isMember("imageB64"));

  const std::string notAnImage =
      postDescribe(port, drogon::utils::base64Encode("hello"), personPrompt);
  Json::Value notAnImageJson;
  REQUIRE(reader.parse(notAnImage, notAnImageJson));
  CHECK(notAnImageJson["status"].asInt() == 422);
  CHECK(notAnImageJson["errors"]["fields"].isMember("image_b64"));

  const auto notFound = request(port, "GET", "/vlm/v1/missing", "");
  CHECK(notFound.status == 404);
  CHECK(envelope(notFound)["errors"]["code"] == "NOT_FOUND");

  const auto notAllowed = request(port, "POST", "/vlm/v1/config", "");
  CHECK(notAllowed.status == 405);
  CHECK(envelope(notAllowed)["errors"]["code"] == "METHOD_NOT_ALLOWED");

  vlm->shutdownEngine();
  const auto down = postDescribe(port, imageB64, personPrompt);
  Json::Value downJson;
  REQUIRE(reader.parse(down, downJson));
  CHECK(downJson["status"].asInt() == 503);
  CHECK(downJson["errors"]["code"] == "VLM_NOT_LOADED");

  vlm->initEngine();
  CHECK(vlm->isEngineLoaded());

  drogon::app().quit();
  runner.join();
  vlm->shutdownEngine();
  llama_backend_free();
  std::remove(kScratchConfig);
}
