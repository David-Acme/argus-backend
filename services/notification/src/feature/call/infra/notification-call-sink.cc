#include "notification-call-sink.hxx"

#include <trantor/utils/Logger.h>

#include <utility>

NotificationCallSink::NotificationCallSink(
    NotificationService::Dependencies dependencies)
    : service_(std::move(dependencies))
{
}

drogon::Task<bool> NotificationCallSink::notify(const CallNotice& notice) const
{
  try {
    const auto outcome = co_await service_.createManyAndEmit(
        {.userIds = {notice.userId},
         .notification = {.userId = notice.userId,
                          .type = notice.type,
                          .title = notice.title,
                          .body = notice.body,
                          .data = notice.data},
         .commandId = notice.commandId});
    co_return outcome.createdCount > 0 || outcome.duplicate;
  }
  catch (const std::exception& error) {
    LOG_WARN << "Call engine: notification " << notice.commandId
             << " failed: " << error.what();
    co_return false;
  }
}
