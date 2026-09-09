#include "health-extras.hxx"

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
      {"pushQueued",
       [&relay] {
         return Json::Value(Json::Value::Int64(relay.pushQueued()));
       }},
      {"pushReceived",
       [&relay] {
         return Json::Value(Json::Value::Int64(relay.pushReceived()));
       }},
      {"pushDropped",
       [&relay] {
         return Json::Value(Json::Value::Int64(relay.pushDropped()));
       }},
      {"pushForwarded",
       [&relay] {
         return Json::Value(Json::Value::Int64(relay.pushForwarded()));
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
         return Json::Value(Json::Value::Int64(client.pushQueued()));
       }},
      {"pushReceived",
       [&client] {
         return Json::Value(Json::Value::Int64(client.pushReceived()));
       }},
      {"pushDropped",
       [&client] {
         return Json::Value(Json::Value::Int64(client.pushDropped()));
       }},
  };
  return status;
}

} // namespace tunnel
