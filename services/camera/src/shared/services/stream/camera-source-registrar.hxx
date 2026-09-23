#pragma once

#include <cstdint>
#include <shared/schemas/camera/camera-schema.hxx>
#include <string>

class ICameraSourceSink
{
public:
  virtual ~ICameraSourceSink() = default;
  virtual bool addSource(const std::string& name, const std::string& url) = 0;
  virtual bool removeSource(const std::string& name) = 0;
};

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

CameraSourceRegistrar& cameraSourceRegistrar();
