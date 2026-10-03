#include "camera-source-registrar.hxx"

#include "go2rtc-manager.hxx"

namespace
{

std::string encodeUserInfo(const std::string& value)
{
  constexpr char kHex[] = "0123456789ABCDEF";
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

std::string sourceName(int64_t cameraId, bool sub)
{
  return sub ? Go2rtcManager::subStreamName(cameraId)
             : Go2rtcManager::streamName(cameraId);
}

class Go2rtcSourceSink : public ICameraSourceSink
{
public:
  bool applySources(const CameraSourceChange& change) override
  {
    Go2rtcSourceChange sources;
    sources.upserts.reserve(change.upserts.size());
    for (const auto& source : change.upserts)
      sources.upserts.push_back({.name = source.name, .url = source.url});
    sources.removals = change.removals;
    return Go2rtcManager::instance().applySources(sources);
  }
};

void collect(const CameraSchema& camera, CameraSourceChange& change)
{
  if (camera.id <= 0)
    return;
  if (!camera.isEnabled) {
    change.removals.push_back(sourceName(camera.id, false));
    change.removals.push_back(sourceName(camera.id, true));
    return;
  }
  change.upserts.push_back({.name = sourceName(camera.id, false),
                            .url = CameraSourceRegistrar::sourceUrl(camera, "stream1")});
  change.upserts.push_back({.name = sourceName(camera.id, true),
                            .url = CameraSourceRegistrar::sourceUrl(camera, "stream2")});
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
  url += camera.ip;
  url += ':';
  url += std::to_string(camera.port > 0 ? camera.port : 554);
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
                      .removals = {sourceName(cameraId, false),
                                   sourceName(cameraId, true)}});
}

CameraSourceRegistrar& cameraSourceRegistrar()
{
  static Go2rtcSourceSink sink;
  static CameraSourceRegistrar registrar(sink);
  return registrar;
}
