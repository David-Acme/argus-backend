#pragma once

#include <cstdint>
#include <shared/schemas/camera/camera-schema.hxx>
#include <string>

// Test seam for the source sink; production rides Go2rtcManager.
class ICameraSourceSink
{
public:
  virtual ~ICameraSourceSink() = default;
  virtual bool addSource(const std::string& name, const std::string& url) = 0;
  virtual bool removeSource(const std::string& name) = 0;
};

// Keeps go2rtc's cam<id>/-sub sources aligned with a camera row.
class CameraSourceRegistrar
{
public:
  explicit CameraSourceRegistrar(ICameraSourceSink& sink) : sink_(sink) {}

  void apply(const CameraSchema& camera) const;
  void remove(int64_t cameraId) const;

  static std::string sourceUrl(const CameraSchema& camera,
                               const std::string& path);

private:
  ICameraSourceSink& sink_;
};

// Process-wide registrar backed by the Go2rtcManager singleton.
CameraSourceRegistrar& cameraSourceRegistrar();
