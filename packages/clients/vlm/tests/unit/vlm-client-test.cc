#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <filesystem>
#include <functional>
#include <json/json.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <vlm/vlm-client.hxx>

namespace
{

const VlmDescribeInput kAsk{.jpeg = "argus",
                            .prompt = "is a person present?",
                            .cameraId = "cam-1"};

constexpr const char* kCaptionBody =
    R"({"status":200,"info":{"caption":"canned caption"},"errors":null})";

struct SeenRequest
{
  std::string method;
  std::string path;
  std::string body;
};

class FakeVlmService
{
public:
  FakeVlmService()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    const auto uploads =
        std::filesystem::temp_directory_path() / "argus-vlm-client-test";
    std::error_code ignored;
    std::filesystem::create_directories(uploads, ignored);
    drogon::app().setUploadPath(uploads.string());
    drogon::app().registerHandler(
        "/vlm/v1/describe",
        [this](const drogon::HttpRequestPtr& request,
               std::function<void(const drogon::HttpResponsePtr&)>&& done) {
          const std::lock_guard<std::mutex> lock(mutex_);
          seen_ = {.method = std::string(request->methodString()),
                   .path = std::string(request->path()),
                   .body = std::string(request->getBody())};
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setStatusCode(status_);
          response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          response->setBody(body_);
          done(response);
        },
        {drogon::Post});
    drogon::app().addListener("127.0.0.1", 0);
    runner_ = std::thread([] { drogon::app().run(); });

    for (int tries = 0; tries < 1000 && !drogon::app().isRunning(); ++tries)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto listeners = drogon::app().getListeners();
    if (drogon::app().isRunning() && !listeners.empty())
      port_ = listeners.front().toPort();
  }

  ~FakeVlmService()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  bool ready() const { return port_ > 0; }

  std::string url() const
  {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  void answer(drogon::HttpStatusCode status, const std::string& body)
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
    body_ = body;
  }

  SeenRequest seen() const
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

private:
  std::thread runner_;
  int port_{0};
  drogon::HttpStatusCode status_{drogon::k200OK};
  std::string body_{kCaptionBody};
  SeenRequest seen_;
  mutable std::mutex mutex_;
};

FakeVlmService& fakeVlmService()
{
  static FakeVlmService service;
  return service;
}

bool parseBody(const std::string& text, Json::Value& body)
{
  Json::Reader reader;
  return reader.parse(text, body);
}

}

TEST_CASE("the describe request carries the JPEG as base64 on the wire")
{
  auto& service = fakeVlmService();
  REQUIRE(service.ready());
  service.answer(drogon::k200OK, kCaptionBody);

  const VlmClient client(service.url());
  const auto result = drogon::sync_wait(client.describe(kAsk));
  REQUIRE(result.has_value());
  CHECK(result->caption == "canned caption");

  const auto seen = service.seen();
  CHECK(seen.method == "POST");
  CHECK(seen.path == "/vlm/v1/describe");

  Json::Value body;
  REQUIRE(parseBody(seen.body, body));
  CHECK(body["image_b64"].asString() == "YXJndXM=");
  CHECK(body["prompt"].asString() == "is a person present?");
  CHECK(body["camera_id"].asString() == "cam-1");

  VlmDescribeInput bare = kAsk;
  bare.prompt.clear();
  bare.cameraId.clear();
  REQUIRE(drogon::sync_wait(client.describe(bare)).has_value());
  REQUIRE(parseBody(service.seen().body, body));
  REQUIRE(body.isMember("prompt"));
  CHECK(body["prompt"].asString().empty());
  CHECK_FALSE(body.isMember("camera_id"));
}

TEST_CASE("the describe envelope decides what the caller gets")
{
  auto& service = fakeVlmService();
  REQUIRE(service.ready());
  const VlmClient client(service.url());

  struct Envelope
  {
    const char* label;
    drogon::HttpStatusCode status;
    const char* body;
  };
  const std::vector<Envelope> kRefused{
      {"a blank caption", drogon::k200OK,
       R"({"status":200,"info":{"caption":""}})"},
      {"a non-object info", drogon::k200OK, R"({"status":200,"info":null})"},
      {"a body that is not JSON", drogon::k200OK, "not json"},
      {"a 503 envelope", drogon::k503ServiceUnavailable,
       R"({"status":503,"errors":{"code":"VLM_NOT_LOADED"}})"},
  };
  for (const auto& one : kRefused) {
    CAPTURE(one.label);
    service.answer(one.status, one.body);
    CHECK_FALSE(drogon::sync_wait(client.describe(kAsk)).has_value());
  }
  service.answer(drogon::k200OK, kCaptionBody);
}

TEST_CASE("describe stops before the wire, and raises when the wire is dead")
{
  REQUIRE(fakeVlmService().ready());

  const VlmClient client("http://127.0.0.1:1");

  VlmDescribeInput empty = kAsk;
  empty.jpeg.clear();
  CHECK_FALSE(drogon::sync_wait(client.describe(empty)).has_value());

  const VlmClient noUrl("");
  CHECK_FALSE(drogon::sync_wait(noUrl.describe(kAsk)).has_value());

  CHECK_THROWS_AS(drogon::sync_wait(client.describe(kAsk)),
                  drogon::HttpException);
}
