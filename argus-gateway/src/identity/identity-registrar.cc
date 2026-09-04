#include "identity-registrar.hxx"

#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>
#include <feature/api/auth/controllers/auth-controller.hxx>
#include <feature/api/invitation/controllers/invitation-controller.hxx>
#include <feature/api/pairing/controllers/pairing-controller.hxx>
#include <feature/api/user/controllers/portrait-preview-controller.hxx>
#include <feature/api/user/controllers/user-controller.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <memory>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
template <typename T>
void requireController(const std::set<std::string>& handlerDescriptions)
{
  if (!drogon::DrClassMap::getSingleInstance<T>())
    throw std::runtime_error(std::string(T::classTypeName())
                             + " is not registered");

  const std::string prefix =
      "HttpController: " + std::string(T::classTypeName()) + "::";
  const auto hasRoutes = std::any_of(handlerDescriptions.begin(),
                                     handlerDescriptions.end(),
                                     [&prefix](const std::string& description) {
                                       return description.rfind(prefix, 0) == 0;
                                     });
  if (!hasRoutes)
    throw std::runtime_error(prefix + " has no routes registered");
}

template <typename T>
void requireFilter()
{
  if (!drogon::DrClassMap::getSingleInstance<T>())
    throw std::runtime_error(std::string(T::classTypeName())
                             + " is not registered");
}
}

IdentityRegistrationStats registerIdentitySurface()
{
  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().registerController(std::make_shared<AuthController>());
  drogon::app().registerController(std::make_shared<InvitationController>());
  drogon::app().registerController(std::make_shared<PairingController>());
  drogon::app().registerController(std::make_shared<UserController>());
  drogon::app().registerController(
      std::make_shared<PortraitPreviewController>());

  std::set<std::string> handlerDescriptions;
  for (const auto& handlerInfo : drogon::app().getHandlersInfo())
    handlerDescriptions.insert(std::get<2>(handlerInfo));

  requireFilter<DeviceFilter>();
  requireFilter<ValidJsonFilter>();
  requireFilter<JwtFilter>();
  requireFilter<RoleFilter>();

  requireController<AuthController>(handlerDescriptions);
  requireController<InvitationController>(handlerDescriptions);
  requireController<PairingController>(handlerDescriptions);
  requireController<UserController>(handlerDescriptions);
  requireController<PortraitPreviewController>(handlerDescriptions);

  return {.controllers = 5, .filters = 4};
}