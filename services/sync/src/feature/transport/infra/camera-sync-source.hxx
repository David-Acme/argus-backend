#pragma once

#include <auth/jwt-filter.hxx>
#include <memory>
#include <sync/syncable.hxx>

enum class CameraSyncTable
{
  Camera,
  CameraStream,
  Zone,
};

class CameraSyncSource
{
public:
  virtual ~CameraSyncSource() = default;

  [[nodiscard]] virtual bool serves(CameraSyncTable table) const = 0;

  [[nodiscard]] virtual std::unique_ptr<Syncable>
  sourceFor(CameraSyncTable table, const JwtContext& ctx) const = 0;
};
