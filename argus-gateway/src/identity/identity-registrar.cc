#include "identity-registrar.hxx"

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
  drogon::app().registerController(std::make_shared<PortraitPreviewController>());

  return {.controllers = 5, .filters = 4};
}