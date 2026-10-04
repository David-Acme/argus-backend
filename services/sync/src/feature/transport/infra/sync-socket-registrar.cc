#include "sync-socket-registrar.hxx"

#include <drogon/drogon.h>
#include <feature/transport/controllers/sync-socket.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

SyncRegistrationStats registerSyncSurface(SyncSurfaceInput input)
{
  const auto socket = std::make_shared<SyncSocket>();
  socket->setForwarder(std::move(input.forwarder));
  socket->setCameraSource(std::move(input.cameraSource));
  socket->setProductivitySource(std::move(input.productivitySource));
  socket->setNotificationSource(std::move(input.notificationSource));
  socket->setIdentitySource(std::move(input.identitySource));
  socket->setUserDirectory(std::move(input.userDirectory));
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

  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<JwtFilter>();

  return {.controllers = 1, .filters = 2};
}
