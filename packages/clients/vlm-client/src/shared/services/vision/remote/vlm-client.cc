#include <shared/services/vision/remote/vlm-client.hxx>

#include <drogon/drogon.h>
#include <shared/utils/base64/base64.hxx>

#include <string>
#include <utility>

VlmClient::VlmClient(std::string baseUrl, double timeoutS)
    : baseUrl_(std::move(baseUrl)), timeoutS_(timeoutS)
{
}

drogon::Task<std::optional<VlmDescribeResult>>
VlmClient::describe(const VlmDescribeInput& input) const
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
