#include <shared/services/onvif/onvif-transport.hxx>

#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>

#include <cstdint>
#include <utility>

OnvifHttpTransport::OnvifHttpTransport(OnvifEndpoint endpoint)
    : endpoint_(std::move(endpoint)),
      client_(drogon::HttpClient::newHttpClient(endpoint_.host, static_cast<uint16_t>(endpoint_.port)))
{
}

OnvifHttpResponse OnvifHttpTransport::post(const OnvifHttpRequest& request)
{
  auto http = drogon::HttpRequest::newHttpRequest();
  http->setMethod(drogon::Post);
  http->setPath(request.path);
  http->setContentTypeString("application/soap+xml; charset=utf-8; action=\"" + request.action + "\"");
  http->setBody(request.body);
  if (!request.authorization.empty())
    http->addHeader("Authorization", request.authorization);
  const auto [result, response] = client_->sendRequest(http, endpoint_.timeoutSeconds);
  if (result != drogon::ReqResult::Ok || !response)
    return {.status = 0,
            .body = {},
            .challenge = {},
            .error = "no ONVIF answer from " + endpoint_.host + ":" + std::to_string(endpoint_.port) +
                     " (" + drogon::to_string(result) + ")"};
  return {.status = static_cast<int>(response->statusCode()),
          .body = std::string(response->body()),
          .challenge = response->getHeader("www-authenticate"),
          .error = {}};
}
