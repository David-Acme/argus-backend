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
// Real evidence that the filter's object code is linked: DrObject-derived
// classes static-register into the class map (getAllClassName) when their
// translation unit is linked. getSingleInstance would fabricate the instance
// instead.
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

SyncRegistrationStats registerSyncSurface(
    std::shared_ptr<SyncForwarder> forwarder,
    std::shared_ptr<CameraSyncSource> cameraSource)
{
  const auto socket = std::make_shared<SyncSocket>();
  socket->setForwarder(std::move(forwarder));
  socket->setCameraSource(std::move(cameraSource));
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
