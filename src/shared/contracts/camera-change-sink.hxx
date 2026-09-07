#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>

// Input of the camera-domain audit publication: the before/after snapshots of
// one persisted camera or zone row. The sink computes the same flat diff the
// legacy SyncAuditService produces.
struct CameraAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Camera};
  Json::Value before;
  Json::Value after;
  std::optional<int64_t> actorId;
};

// Substrate of the camera-domain change events: the owning service installs
// one implementation at boot, before it serves. Since F6-2 the only binder is
// argus-camera, which funnels the payloads over NATS (Ruling Y).
class CameraChangeSink
{
public:
  virtual ~CameraChangeSink() = default;

  virtual void emitModule(TableName table, const SocketEmitDto& body) const = 0;

  virtual drogon::Task<void>
  publishAudit(const CameraAuditInput& input) const = 0;
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
