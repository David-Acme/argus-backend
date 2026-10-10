#include "camera-driver.hxx"

#include "tapo-driver.hxx"

#include <mutex>
#include <text/sha256.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_map>

namespace
{
struct Entry
{
  std::string fingerprint;
  std::shared_ptr<ICameraDriver> driver;
};

std::mutex gMutex;
std::unordered_map<int64_t, Entry> gDrivers;

std::string fingerprintOf(const CameraSchema& camera)
{
  return argus::hash::sha256Hex(camera.ip + '|' + std::to_string(camera.port) + '|' +
                                cameraDriverToString(camera.driver) + '|' + camera.username +
                                '|' + camera.password + '|' + camera.cloudUsername + '|' +
                                camera.cloudPassword);
}
}

TalkLineOpen ICameraDriver::talkLine()
{
  return {.line = nullptr, .error = "This camera has no speaker Argus can reach"};
}

Json::Value ICameraDriver::controlStatus() const
{
  return {};
}

CameraDriverRegistry& CameraDriverRegistry::instance()
{
  static CameraDriverRegistry registry;
  return registry;
}

std::shared_ptr<ICameraDriver> CameraDriverRegistry::driverFor(const CameraSchema& camera)
{
  std::lock_guard<std::mutex> lock(gMutex);
  const std::string fingerprint = fingerprintOf(camera);
  const auto it = gDrivers.find(camera.id);
  if (it != gDrivers.end() &&
      (it->second.fingerprint.empty() || it->second.fingerprint == fingerprint))
    return it->second.driver;

  std::shared_ptr<ICameraDriver> driver;
  switch (camera.driver) {
    case CameraDriver::Tapo:
      driver = std::make_shared<TapoDriver>(camera);
      break;
    case CameraDriver::Onvif:
    case CameraDriver::Rtsp:
      LOG_WARN << "Camera driver not implemented yet: "
               << cameraDriverToString(camera.driver);
      return nullptr;
  }

  gDrivers[camera.id] = {.fingerprint = fingerprint, .driver = driver};
  return driver;
}

void CameraDriverRegistry::forget(int64_t cameraId)
{
  std::lock_guard<std::mutex> lock(gMutex);
  gDrivers.erase(cameraId);
}

void CameraDriverTestAccess::install(
    int64_t cameraId, const std::shared_ptr<ICameraDriver>& driver)
{
  std::lock_guard<std::mutex> lock(gMutex);
  gDrivers[cameraId] = {.fingerprint = std::string(), .driver = driver};
}
