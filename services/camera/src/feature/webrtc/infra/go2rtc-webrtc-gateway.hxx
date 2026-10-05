#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

struct Go2rtcWebRtcExchange
{
  std::string source;
  std::string offer;
  std::string tag;
};

struct Go2rtcWebRtcConsumer
{
  std::string stream;
  std::string userAgent;
};

class Go2rtcWebRtcGateway
{
public:
  drogon::Task<std::optional<std::string>> exchange(Go2rtcWebRtcExchange request) const;
  drogon::Task<std::optional<Json::Value>> streams() const;
};

namespace go2rtc_streams
{
std::vector<Go2rtcWebRtcConsumer> webrtcConsumers(const Json::Value& streams);
bool carriesAac(const Json::Value& streams, const std::string& source);
}
