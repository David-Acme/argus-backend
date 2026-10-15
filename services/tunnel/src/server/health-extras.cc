#include "health-extras.hxx"

#include <http/health-controller.hxx>

namespace tunnel
{

HealthStatus relayHealthStatus(TunnelRelay& relay)
{
  HealthStatus status;
  status.serviceName = "argus-relay";
  status.extras = {
      {"homeConnected", [&relay] { return Json::Value(relay.hasHome()); }},
      {"activeStreams",
       [&relay] {
         return Json::Value(static_cast<int>(relay.streamCount()));
       }},
  };
  return status;
}

HealthStatus clientHealthStatus(TunnelClient& client)
{
  HealthStatus status;
  status.serviceName = "argus-tunnel-client";
  status.extras = {
      {"homeConnected",
       [&client] { return Json::Value(client.homeConnected()); }},
      {"activeStreams",
       [&client] {
         return Json::Value(static_cast<int>(client.streamCount()));
       }},
      {"pushQueued",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushQueued()));
       }},
      {"pushReceived",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushReceived()));
       }},
      {"pushDropped",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushDropped()));
       }},
  };
  return status;
}

HealthStatus linkHealthStatus(link::LinkClient& client)
{
  HealthStatus status;
  status.serviceName = "argus-tunnel-client";
  status.extras = {
      {"homeConnected", [&client] { return Json::Value(client.connected()); }},
      {"linkDraining", [&client] { return Json::Value(client.draining()); }},
      {"keepaliveActive",
       [&client] { return Json::Value(client.keepaliveActive()); }},
      {"activeStreams",
       [&client] {
         return Json::Value(static_cast<int>(client.streamCount()));
       }},
      {"nodeCapabilities",
       [&client] {
         return Json::Value(static_cast<int>(client.remoteCapabilities()));
       }},
      {"reconnectAttempts",
       [&client] { return Json::Value(client.reconnectAttempts()); }},
      {"lastReconnectDelayMs",
       [&client] { return Json::Value(client.lastReconnectDelayMs()); }},
      {"pushQueued",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushQueued()));
       }},
      {"pushReceived",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushReceived()));
       }},
      {"pushDropped",
       [&client] {
         return Json::Value(static_cast<Json::Int64>(client.pushDropped()));
       }},
  };
  return status;
}

}
