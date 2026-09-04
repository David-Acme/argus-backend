#include "sync-registrar.hxx"

#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>
#include <feature/socket/sync/socket/sync-socket.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <stdexcept>
#include <string>

namespace
{
template <typename T>
void requireFilter()
{
  if (!drogon::DrClassMap::getSingleInstance<T>())
    throw std::runtime_error(std::string(T::classTypeName())
                             + " is not registered");
}
} // namespace

SyncRegistrationStats registerSyncSurface(
    std::shared_ptr<SyncForwarder> forwarder)
{
  const auto socket = std::make_shared<SyncSocket>();
  socket->setForwarder(std::move(forwarder));
  drogon::app().registerController(socket);

  bool registered = false;
  for (const auto& handlerInfo : drogon::app().getHandlersInfo()) {
    const auto& description = std::get<2>(handlerInfo);
    if (description == "WebsocketController: SyncSocket") {
      registered = true;
      break;
    }
  }
  if (!registered)
    throw std::runtime_error("WebsocketController: SyncSocket"
                             " has no routes registered");
  if (!drogon::DrClassMap::getSingleInstance<SyncSocket>())
    throw std::runtime_error("SyncSocket is not registered");

  requireFilter<DeviceFilter>();
  requireFilter<JwtFilter>();

  return {.controllers = 1, .filters = 2};
}