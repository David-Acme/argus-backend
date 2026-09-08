#pragma once

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>

// Before/after snapshots of one camera or zone row; the sink diffs them.
struct CameraAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Camera};
  Json::Value before;
  Json::Value after;
  std::optional<int64_t> actorId;
};

// Camera-domain change sink; argus-camera installs the NATS funnel at boot.
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
