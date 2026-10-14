#include <shared/services/onvif/onvif-transport.hxx>

#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

#include <cstdint>
#include <utility>

namespace
{
drogon::HttpRequestPtr requestOf(const OnvifHttpRequest& request)
{
  auto http = drogon::HttpRequest::newHttpRequest();
  http->setMethod(drogon::Post);
  http->setPath(request.path);
  http->setPathEncode(false);
  http->setContentTypeString("application/soap+xml; charset=utf-8; action=\"" + request.action + "\"");
  http->setBody(request.body);
  if (!request.authorization.empty())
    http->addHeader("Authorization", request.authorization);
  return http;
}

OnvifHttpResponse answerOf(const drogon::HttpResponsePtr& response)
{
  return {.status = static_cast<int>(response->statusCode()),
          .body = std::string(response->body()),
          .challenge = response->getHeader("www-authenticate"),
          .error = {}};
}

OnvifHttpResponse silenceOf(const OnvifEndpoint& endpoint, drogon::ReqResult result)
{
  return {.status = 0,
          .body = {},
          .challenge = {},
          .error = "no ONVIF answer from " + endpoint.host + ":" + std::to_string(endpoint.port) + " (" +
                   drogon::to_string(result) + ")"};
}

double timeoutOf(const OnvifHttpRequest& request, const OnvifEndpoint& endpoint)
{
  return request.timeoutSeconds > 0.0 ? request.timeoutSeconds : endpoint.timeoutSeconds;
}
}

OnvifHttpTransport::OnvifHttpTransport(OnvifEndpoint endpoint)
    : endpoint_(std::move(endpoint)),
      client_(drogon::HttpClient::newHttpClient(endpoint_.host, static_cast<uint16_t>(endpoint_.port)))
{
}

OnvifHttpResponse OnvifHttpTransport::post(const OnvifHttpRequest& request)
{
  const auto [result, response] = client_->sendRequest(requestOf(request), timeoutOf(request, endpoint_));
  if (result != drogon::ReqResult::Ok || !response)
    return silenceOf(endpoint_, result);
  return answerOf(response);
}

OnvifAsyncHttpTransport::OnvifAsyncHttpTransport(OnvifEndpoint endpoint, trantor::EventLoop* loop)
    : endpoint_(std::move(endpoint)),
      client_(drogon::HttpClient::newHttpClient(endpoint_.host, static_cast<uint16_t>(endpoint_.port), false, loop))
{
}

drogon::Task<OnvifHttpResponse> OnvifAsyncHttpTransport::send(OnvifHttpRequest request)
{
  try {
    const auto response = co_await client_->sendRequestCoro(requestOf(request), timeoutOf(request, endpoint_));
    if (!response)
      co_return silenceOf(endpoint_, drogon::ReqResult::BadResponse);
    co_return answerOf(response);
  }
  catch (const drogon::HttpException& failure) {
    co_return silenceOf(endpoint_, failure.code());
  }
}
