#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/module-audit-event.hxx>
#include <sync/module-emit.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>

class CameraChangeSink
{
public:
  virtual ~CameraChangeSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const = 0;

  [[nodiscard]] virtual drogon::Task<void>
  publishAudit(const ModuleAuditInput& input) const = 0;
};

namespace camera_change
{
inline const CameraChangeSink*& sink()
{
  static const CameraChangeSink* instance = nullptr;
  return instance;
}

inline void setSink(const CameraChangeSink* value)
{
  sink() = value;
}

inline const CameraChangeSink* getSink()
{
  return sink();
}
}
