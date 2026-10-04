#include "sync-call-signal.hxx"

#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

SyncCallSignal::SyncCallSignal(std::shared_ptr<const SyncClient> client)
    : client_(std::move(client))
{
}

bool SyncCallSignal::emit(const CallSignalInput& input) const
{
  if (!client_)
    return false;
  SocketEmitDto frame;
  frame.operation = input.operation;
  frame.option = TableName::Notification;
  frame.obj = input.info;
  const bool sent = client_->emitToUser(input.userId, frame);
  if (!sent)
    LOG_WARN << "Call engine: argus-sync refused the "
             << syncOperationToString(input.operation) << " frame for user "
             << input.userId;
  return sent;
}
