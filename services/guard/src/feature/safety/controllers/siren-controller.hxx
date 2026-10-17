#pragma once

#include <feature/safety/services/siren-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <memory>

class SirenController : public drogon::HttpController<SirenController, false>
{
public:
  explicit SirenController(std::shared_ptr<const SirenService> service);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(SirenController::siren, "/guard/siren", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> siren(drogon::HttpRequestPtr req);

private:
  std::shared_ptr<const SirenService> service_;
};
