#include "camera-source-registrar.hxx"

#include "go2rtc-manager.hxx"

#include <array>
#include <string_view>

#include <shared/utils/network-address/private-address.hxx>
#include <shared/vocabulary/camera-stream-paths.hxx>

namespace
{
constexpr std::array kStreams{CameraStream::Main, CameraStream::Sub};

std::string encodeUserInfo(const std::string& value)
{
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    const bool unreserved = (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

class Go2rtcSourceSink : public ICameraSourceSink
{
public:
  bool applySources(const CameraSourceChange& change) override
  {
    Go2rtcSourceChange sources;
    sources.upserts.reserve(change.upserts.size());
    for (const auto& source : change.upserts)
      sources.upserts.push_back(
          {.name = source.name, .url = source.url, .preload = source.preload});
    sources.removals = change.removals;
    return Go2rtcManager::instance().applySources(sources);
  }
};

void collect(const CameraSchema& camera, CameraSourceChange& change)
{
  if (camera.id <= 0)
    return;
  if (!camera.isEnabled) {
    for (const auto stream : kStreams)
      change.removals.push_back(Go2rtcManager::sourceName(camera.id, stream));
    return;
  }
  const CameraStreamPaths paths = camera_stream_paths::of(camera.config);
  for (const auto stream : kStreams)
    change.upserts.push_back(
        {.name = Go2rtcManager::sourceName(camera.id, stream),
         .url = CameraSourceRegistrar::sourceUrl(
             camera, stream == CameraStream::Main ? paths.main : paths.sub),
         .preload = camera_stream_role::isWarm(stream)});
}

}

std::string CameraSourceRegistrar::sourceUrl(const CameraSchema& camera,
                                             const std::string& path)
{
  std::string url = "rtsp://";
  if (!camera.username.empty()) {
    url += encodeUserInfo(camera.username);
    url += ':';
    url += encodeUserInfo(camera.password);
    url += '@';
  }
  url += network_address::urlHost(camera.ip);
  url += ':';
  url += std::to_string(camera.port > 0 ? camera.port : 554);
  if (!path.starts_with('/'))
    url += '/';
  url += path;
  return url;
}

void CameraSourceRegistrar::apply(const CameraSchema& camera) const
{
  applyAll({camera});
}

void CameraSourceRegistrar::applyAll(const std::vector<CameraSchema>& cameras) const
{
  CameraSourceChange change;
  for (const auto& camera : cameras)
    collect(camera, change);
  if (!change.upserts.empty() || !change.removals.empty())
    sink_.applySources(change);
}

void CameraSourceRegistrar::remove(int64_t cameraId) const
{
  if (cameraId <= 0)
    return;
  sink_.applySources({.upserts = {},
                      .removals = {Go2rtcManager::sourceName(cameraId, CameraStream::Main),
                                   Go2rtcManager::sourceName(cameraId, CameraStream::Sub)}});
}

CameraSourceRegistrar& cameraSourceRegistrar()
{
  static Go2rtcSourceSink sink;
  static CameraSourceRegistrar registrar(sink);
  return registrar;
}
