#include <sync/user-change-sink.hxx>

#include <shared/services/socket/socket-user-change-sink.hxx>

void installUserChangeSink()
{
  static const SocketUserChangeSink sink;
  user_change::setNotificationSink(&sink);
}
