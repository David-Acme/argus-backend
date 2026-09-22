#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <sync/module-audit-event.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>

// Camera-domain change sink; argus-camera installs the NATS funnel at boot.
class CameraChangeSink
{
public:
  virtual ~CameraChangeSink() = default;

  [[nodiscard]] virtual drogon::Task<void>
  emitModule(TableName table, const SocketEmitDto& body) const = 0;

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
} // namespace camera_change
