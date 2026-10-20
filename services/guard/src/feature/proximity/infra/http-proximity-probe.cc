#include "http-proximity-probe.hxx"

#include <feature/proximity/infra/proximity-readings.hxx>

#include <drogon/HttpClient.h>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

HttpProximityProbe::HttpProximityProbe(HttpProximityProbeConfig config) : config_(std::move(config)) {}

drogon::Task<std::optional<Json::Value>> HttpProximityProbe::fetch(Fetch request) const
{
  if (request.host.empty() || request.port <= 0)
    co_return std::nullopt;
  auto http = drogon::HttpRequest::newHttpRequest();
  http->setMethod(drogon::Get);
  http->setPath(request.path);
  if (request.bearer) {
    if (config_.homeAssistantToken.empty())
      co_return std::nullopt;
    http->addHeader("Authorization", "Bearer " + config_.homeAssistantToken);
  }
  try {
    const auto client =
        drogon::HttpClient::newHttpClient("http://" + request.host + ":" + std::to_string(request.port));
    const auto response = co_await client->sendRequestCoro(http, config_.timeoutSeconds);
    if (response->statusCode() != drogon::k200OK)
      co_return std::nullopt;
    const Json::Value body = json_util::fromString(std::string(response->body()));
    if (!body.isObject())
      co_return std::nullopt;
    co_return body;
  }
  catch (const std::exception& error) {
    LOG_DEBUG << "Proximity: " << request.host << request.path << " did not answer: " << error.what();
    co_return std::nullopt;
  }
}

drogon::Task<std::optional<std::vector<BleHeard>>> HttpProximityProbe::readBle(ProximityReceiverRow receiver) const
{
  const ProximityRequestPath path = bleRequestPath(config_.bleEntity);
  const auto body =
      co_await fetch({.host = receiver.host, .port = receiver.port, .path = path.path, .bearer = path.bearer});
  if (!body)
    co_return std::nullopt;
  co_return bleHeardOf(*body);
}

drogon::Task<std::optional<bool>> HttpProximityProbe::readRadar(ProximitySensorRow sensor) const
{
  const ProximityRequestPath path = radarRequestPath(sensor);
  const auto body = co_await fetch({.host = sensor.host, .port = sensor.port, .path = path.path, .bearer = path.bearer});
  if (!body)
    co_return std::nullopt;
  co_return radarPresenceOf(*body);
}

drogon::Task<std::optional<std::vector<RadarPoint>>> HttpProximityProbe::readTargets(ProximitySensorRow sensor) const
{
  const ProximityRequestPath path = radarTargetsPath({.sensor = sensor, .suffix = config_.targetsSuffix});
  const auto body = co_await fetch({.host = sensor.host, .port = sensor.port, .path = path.path, .bearer = path.bearer});
  if (!body)
    co_return std::nullopt;
  co_return radarTargetsOf(*body);
}
