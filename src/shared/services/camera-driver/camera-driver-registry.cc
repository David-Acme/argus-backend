#include "camera-driver.hxx"

#include "tapo-driver.hxx"

#include <mutex>
#include <trantor/utils/Logger.h>
#include <unordered_map>

namespace
{
std::mutex gMutex;
std::unordered_map<int64_t, std::shared_ptr<ICameraDriver>> gDrivers;
} // namespace

CameraDriverRegistry& CameraDriverRegistry::instance()
{
  static CameraDriverRegistry registry;
  return registry;
}

std::shared_ptr<ICameraDriver> CameraDriverRegistry::driverFor(const CameraSchema& camera)
{
  std::lock_guard<std::mutex> lock(gMutex);
  const auto it = gDrivers.find(camera.id);
  if (it != gDrivers.end())
    return it->second;

  std::shared_ptr<ICameraDriver> driver;
  switch (camera.driver) {
    case CameraDriver::Tapo:
      driver = std::make_shared<TapoDriver>(camera);
      break;
    // Kept explicit so a new integration is a compile error until it is wired.
    case CameraDriver::Onvif:
    case CameraDriver::Rtsp:
      LOG_WARN << "Camera driver not implemented yet: "
               << cameraDriverToString(camera.driver);
      return nullptr;
  }

  gDrivers[camera.id] = driver;
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
  gDrivers[cameraId] = driver;
}
