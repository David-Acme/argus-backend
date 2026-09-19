#include "sync-registrar.hxx"

#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>
#include <feature/socket/sync/socket/sync-socket.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
// Evidence the filter's object code is linked: getSingleInstance would fabricate it.
template <typename T>
void requireLinkedFilter()
{
  const auto names = drogon::DrClassMap::getAllClassName();
  if (std::find(names.begin(), names.end(), T::classTypeName()) ==
      names.end())
    throw std::runtime_error(std::string(T::classTypeName())
                             + " is not linked into this binary");
}
} // namespace

SyncRegistrationStats registerSyncSurface(SyncSurfaceInput input)
{
  const auto socket = std::make_shared<SyncSocket>();
  socket->setForwarder(std::move(input.forwarder));
  socket->setCameraSource(std::move(input.cameraSource));
  socket->setProductivitySource(std::move(input.productivitySource));
  socket->setNotificationSource(std::move(input.notificationSource));
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

  requireLinkedFilter<DeviceFilter>();
  requireLinkedFilter<JwtFilter>();

  return {.controllers = 1, .filters = 2};
}
