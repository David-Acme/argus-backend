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
  std::string name = Go2rtcManager::streamName(cameraId);
  if (sub)
    name += "-sub";
  return name;
}

class Go2rtcSourceSink : public ICameraSourceSink
{
public:
  bool addSource(const std::string& name, const std::string& url) override
  {
    return Go2rtcManager::instance().addSource(
        Go2rtcSource{.name = name, .url = url});
  }

  bool removeSource(const std::string& name) override
  {
    return Go2rtcManager::instance().removeSource(name);
  }
};

} // namespace

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
  if (camera.id <= 0)
    return;
  if (!camera.isEnabled) {
    remove(camera.id);
    return;
  }
  sink_.addSource(sourceName(camera.id, false), sourceUrl(camera, "stream1"));
  sink_.addSource(sourceName(camera.id, true), sourceUrl(camera, "stream2"));
}

void CameraSourceRegistrar::remove(int64_t cameraId) const
{
  if (cameraId <= 0)
    return;
  sink_.removeSource(sourceName(cameraId, false));
  sink_.removeSource(sourceName(cameraId, true));
}

CameraSourceRegistrar& cameraSourceRegistrar()
{
  static Go2rtcSourceSink sink;
  static CameraSourceRegistrar registrar(sink);
  return registrar;
}
