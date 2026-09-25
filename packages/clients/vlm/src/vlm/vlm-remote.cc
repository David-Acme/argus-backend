#include "vlm-remote.hxx"

#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <runtime/blocking-task.hxx>
#include <text/base64.hxx>
#include <vlm/vlm-client.hxx>

#include <chrono>
#include <string>
#include <utility>

VlmHttpClient::VlmHttpClient(std::string baseUrl, double timeoutS)
    : baseUrl_(std::move(baseUrl)), timeoutS_(timeoutS)
{
}

drogon::Task<std::optional<VlmDescribeResult>>
VlmHttpClient::describe(const VlmDescribeInput& input) const
{
  if (input.jpeg.empty() || baseUrl_.empty())
    co_return std::nullopt;

  Json::Value body(Json::objectValue);
  body["image_b64"] = base64::encode(input.jpeg);
  body["prompt"] = input.prompt;
  if (!input.cameraId.empty())
    body["camera_id"] = input.cameraId;

  auto request = drogon::HttpRequest::newHttpJsonRequest(body);
  request->setMethod(drogon::Post);
  request->setPath("/vlm/v1/describe");

  auto client = drogon::HttpClient::newHttpClient(baseUrl_);
  const auto response = co_await client->sendRequestCoro(request, timeoutS_);
  if (!response || response->getStatusCode() != drogon::k200OK)
    co_return std::nullopt;

  const auto json = response->getJsonObject();
  if (!json)
    co_return std::nullopt;
  const Json::Value& info = (*json)["info"];
  if (!info.isObject())
    co_return std::nullopt;
  const std::string caption = info.get("caption", "").asString();
  if (caption.empty())
    co_return std::nullopt;
  co_return VlmDescribeResult{.caption = caption};
}

VlmClient::VlmClient(std::string baseUrl, double timeoutS)
    : baseUrl_(std::move(baseUrl)), timeoutS_(timeoutS)
{
}

std::shared_ptr<argus::vlm::Client> VlmClient::rpcClient() const
{
  const auto target = ConfigService::getString("vlm.grpc_target");
  if (target.empty())
    return {};
  auto cached = rpcCache_.load();
  while (!cached || cached->target != target) {
    const auto seconds = static_cast<std::int64_t>(timeoutS_ * 1000.0);
    auto built = std::make_shared<RpcCache>(RpcCache{
        .target = target,
        .client = std::make_shared<argus::vlm::Client>(argus::vlm::ClientConfig{
            .target = target,
            .credential = ConfigService::getString("vlm.grpc_credential"),
            .timeout = std::chrono::milliseconds(seconds)})});
    if (rpcCache_.compare_exchange_weak(cached, built))
      return built->client;
  }
  return cached->client;
}

drogon::Task<std::optional<VlmDescribeResult>>
VlmClient::describe(const VlmDescribeInput& input) const
{
  if (input.jpeg.empty())
    co_return std::nullopt;

  std::shared_ptr<argus::vlm::Client> client;
  try {
    client = rpcClient();
  }
  catch (const ResponseException&) {
    co_return std::nullopt;
  }

  if (client) {
    co_return co_await BlockingTask<std::optional<VlmDescribeResult>>(
        [client, input]() -> std::optional<VlmDescribeResult> {
          try {
            return VlmDescribeResult{.caption = client->describe(
                argus::vlm::DescribeInput{.jpeg = input.jpeg,
                                          .prompt = input.prompt,
                                          .cameraId = input.cameraId,
                                          .maxTokens = 0})};
          }
          catch (const ResponseException&) {
            return std::nullopt;
          }
        });
  }

  co_return co_await VlmHttpClient(baseUrl_, timeoutS_).describe(input);
}

bool VlmClient::remote() const
{
  return !ConfigService::getString("vlm.grpc_target").empty() ||
         !baseUrl_.empty();
}
