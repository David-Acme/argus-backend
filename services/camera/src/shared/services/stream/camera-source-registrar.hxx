#pragma once

#include <cstdint>
#include <shared/schemas/camera/camera-schema.hxx>
#include <string>
#include <vector>

struct CameraSource
{
  std::string name;
  std::string url;
  bool preload{false};
};

struct CameraSourceChange
{
  std::vector<CameraSource> upserts;
  std::vector<std::string> removals;
};

class ICameraSourceSink
{
public:
  virtual ~ICameraSourceSink() = default;
  virtual bool applySources(const CameraSourceChange& change) = 0;
};

class CameraSourceRegistrar
{
public:
  explicit CameraSourceRegistrar(ICameraSourceSink& sink) : sink_(sink) {}

  void apply(const CameraSchema& camera) const;
  void applyAll(const std::vector<CameraSchema>& cameras) const;
  void remove(int64_t cameraId) const;

  static std::string sourceUrl(const CameraSchema& camera,
                               const std::string& path);

private:
  ICameraSourceSink& sink_;
};

CameraSourceRegistrar& cameraSourceRegistrar();
