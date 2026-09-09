#pragma once

#include <filter/jwt/jwt-filter.hxx>
#include <memory>
#include <shared/contracts/syncable.hxx>

enum class CameraSyncTable
{
  Camera,
  CameraStream,
  Zone,
};

// Pull source for the camera-domain sync tables.
class CameraSyncSource
{
public:
  virtual ~CameraSyncSource() = default;

  virtual bool serves(CameraSyncTable table) const = 0;

  virtual std::unique_ptr<Syncable>
  sourceFor(CameraSyncTable table, const JwtContext& ctx) const = 0;
};
