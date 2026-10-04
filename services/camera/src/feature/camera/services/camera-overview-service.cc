#include "camera-overview-service.hxx"

#include <utility>

#include <auth/role-access.hxx>
#include <shared/services/stream/camera-live-board.hxx>
#include <feature/camera/infra/rtsp-probe.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>

#include <drogon/HttpClient.h>
#include <trantor/utils/Logger.h>

namespace
{
constexpr double kGo2rtcTimeoutSeconds = 2.0;

const Json::Value* videoReceiver(const Json::Value& producer)
{
  for (const auto& receiver : producer["receivers"]) {
    if (receiver["codec"].get("codec_type", "").asString() == "video")
      return &receiver;
  }
  return nullptr;
}
}

int CameraOverviewService::kbpsOf(const std::string& stream, int64_t bytes) const
{
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(samplesMutex_);
  auto& sample = samples_[stream];
  if (sample.bytes == 0 || bytes < sample.bytes) {
    sample = {.bytes = bytes, .at = now, .kbps = 0};
    return 0;
  }
  const auto elapsed = std::chrono::duration<double>(now - sample.at).count();
  if (elapsed < 1.0)
    return sample.kbps;
  const int kbps = static_cast<int>(static_cast<double>(bytes - sample.bytes) * 8.0 / elapsed / 1000.0);
  sample = {.bytes = bytes, .at = now, .kbps = kbps};
  return kbps;
}

drogon::Task<Json::Value> CameraOverviewService::streamStats() const
{
  Json::Value out(Json::objectValue);
  try {
    const auto client = drogon::HttpClient::newHttpClient(Go2rtcManager::instance().apiBase());
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setPath("/api/streams");
    const auto response = co_await client->sendRequestCoro(request, kGo2rtcTimeoutSeconds);
    if (!response || response->statusCode() != drogon::k200OK)
      co_return out;
    const auto streams = response->getJsonObject();
    if (!streams || !streams->isObject())
      co_return out;
    for (const auto& name : streams->getMemberNames()) {
      const Json::Value& producers = (*streams)[name]["producers"];
      if (!producers.isArray() || producers.empty())
        continue;
      const Json::Value& producer = producers[0];
      const Json::Value* video = videoReceiver(producer);
      if (video == nullptr)
        continue;
      const auto sdp = rtsp_probe::readSdp(producer.get("sdp", "").asString());
      Json::Value stats(Json::objectValue);
      stats["codec"] = sdp.videoCodec;
      stats["profile"] = (*video)["codec"].get("profile", "").asString();
      stats["audio"] = sdp.audioCodec;
      stats["width"] = sdp.width;
      stats["height"] = sdp.height;
      stats["fps"] = sdp.fps;
      stats["kbps"] = kbpsOf(name, video->get("bytes", static_cast<Json::Int64>(0)).asInt64());
      out[name] = stats;
    }
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera overview: go2rtc stats unavailable (" << error.what() << ")";
  }
  co_return out;
}

drogon::Task<Json::Value> CameraOverviewService::overview(UserRole role) const
{
  const bool readsEvents = role_access::hasAccess(
      {.role = role, .table = TableName::Event, .perm = RolePermission::Read});
  const auto cameras = co_await repository_.findEnabled();
  const auto board = CameraLiveBoard::instance().snapshot();
  const auto viewers = StreamHub::instance().viewersByCamera();
  const Json::Value streams = co_await streamStats();

  Json::Value rows(Json::arrayValue);
  for (const auto& camera : cameras) {
    Json::Value row(Json::objectValue);
    row["id"] = static_cast<Json::Int64>(camera.id);
    const auto state = board.find(camera.id);
    const bool known = state != board.end();
    row["lastSeenAt"] = static_cast<Json::Int64>(known ? state->second.lastSeenMs : 0);
    row["sampledAt"] = static_cast<Json::Int64>(known ? state->second.sampledMs : 0);
    row["health"] = known ? state->second.health : std::string("unknown");
    row["width"] = known ? state->second.width : 0;
    row["height"] = known ? state->second.height : 0;
    const auto watching = viewers.find(camera.id);
    row["viewers"] = watching == viewers.end() ? 0 : watching->second;
    const std::string analysis =
        Go2rtcManager::sourceFor(camera.id, CameraStreamRole::Analysis);
    row["stream"] = streams.isMember(analysis) ? streams[analysis] : Json::Value();
    row["mainActive"] =
        streams.isMember(Go2rtcManager::sourceName(camera.id, CameraStream::Main));
    row["lastEvent"] = readsEvents && known && state->second.lastEvent
                           ? state->second.lastEvent->toJson()
                           : Json::Value();
    rows.append(row);
  }

  Json::Value events(Json::arrayValue);
  if (readsEvents) {
    for (const auto& event : CameraLiveBoard::instance().recentEvents(kRecentEvents))
      events.append(event.toJson());
  }

  Json::Value out(Json::objectValue);
  out["cameras"] = std::move(rows);
  out["events"] = std::move(events);
  co_return out;
}
