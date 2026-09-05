#include "socket-camera-change-sink.hxx"

void SocketCameraChangeSink::emitModule(TableName table,
                                        const SocketEmitDto& body) const
{
  socketService_.emitModule(table, body);
}

drogon::Task<void> SocketCameraChangeSink::publishAudit(
    const CameraAuditInput& input) const
{
  co_await syncAuditService_.publishModule({
      .recordId = input.recordId,
      .tableName = input.tableName,
      .before = input.before,
      .after = input.after,
      .actorId = input.actorId,
  });
  co_return;
}