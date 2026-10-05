#include "go2rtc-webrtc-gateway.hxx"

#include <shared/services/stream/go2rtc-manager.hxx>

#include <drogon/HttpClient.h>
#include <trantor/utils/Logger.h>

#include <string_view>

namespace
{
constexpr double kExchangeTimeoutSeconds = 6.0;
constexpr double kStreamsTimeoutSeconds = 2.0;
constexpr std::string_view kWebRtcFormat = "webrtc";

bool namesAac(std::string_view media)
{
  return media.starts_with("audio") &&
         (media.find("MPEG4-GENERIC") != std::string_view::npos ||
          media.find("AAC") != std::string_view::npos);
}
}

drogon::Task<std::optional<std::string>>
Go2rtcWebRtcGateway::exchange(Go2rtcWebRtcExchange request) const
{
  try {
    const auto client = drogon::HttpClient::newHttpClient(Go2rtcManager::instance().apiBase());
    auto http = drogon::HttpRequest::newHttpRequest();
    http->setMethod(drogon::Post);
    http->setPath("/api/webrtc");
    http->setParameter("src", request.source);
    http->setContentTypeString("application/sdp");
    http->addHeader("User-Agent", request.tag);
    http->setBody(std::move(request.offer));
    const auto response = co_await client->sendRequestCoro(http, kExchangeTimeoutSeconds);
    const auto status = response ? response->statusCode() : drogon::k500InternalServerError;
    if (status != drogon::k201Created && status != drogon::k200OK) {
      LOG_WARN << "Camera WebRTC: go2rtc refused the offer for " << request.source << " ("
               << static_cast<int>(status) << ")";
      co_return std::nullopt;
    }
    std::string answer(response->body());
    if (!answer.starts_with("v=0"))
      co_return std::nullopt;
    co_return answer;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera WebRTC: go2rtc unreachable (" << error.what() << ")";
  }
  co_return std::nullopt;
}

drogon::Task<std::optional<Json::Value>> Go2rtcWebRtcGateway::streams() const
{
  try {
    const auto client = drogon::HttpClient::newHttpClient(Go2rtcManager::instance().apiBase());
    auto http = drogon::HttpRequest::newHttpRequest();
    http->setPath("/api/streams");
    const auto response = co_await client->sendRequestCoro(http, kStreamsTimeoutSeconds);
    if (!response || response->statusCode() != drogon::k200OK)
      co_return std::nullopt;
    const auto json = response->getJsonObject();
    if (!json || !json->isObject())
      co_return std::nullopt;
    co_return *json;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera WebRTC: go2rtc streams unavailable (" << error.what() << ")";
  }
  co_return std::nullopt;
}

namespace go2rtc_streams
{
std::vector<Go2rtcWebRtcConsumer> webrtcConsumers(const Json::Value& streams)
{
  std::vector<Go2rtcWebRtcConsumer> consumers;
  if (!streams.isObject())
    return consumers;
  for (const auto& name : streams.getMemberNames()) {
    const Json::Value& list = streams[name]["consumers"];
    if (!list.isArray())
      continue;
    for (const auto& consumer : list) {
      if (!consumer.isObject() ||
          !consumer.get("format_name", "").asString().starts_with(kWebRtcFormat))
        continue;
      consumers.push_back(
          {.stream = name, .userAgent = consumer.get("user_agent", "").asString()});
    }
  }
  return consumers;
}

bool carriesAac(const Json::Value& streams, const std::string& source)
{
  if (!streams.isObject() || !streams.isMember(source))
    return false;
  for (const auto& producer : streams[source]["producers"]) {
    for (const auto& media : producer["medias"]) {
      if (media.isString() && namesAac(media.asString()))
        return true;
    }
  }
  return false;
}
}
