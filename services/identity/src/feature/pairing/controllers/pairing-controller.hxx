#pragma once

#include <feature/pairing/services/pairing-feature-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class PairingController : public drogon::HttpController<PairingController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(PairingController::pair, "/pairing", drogon::Post,
                "DeviceFilter", "ValidJsonFilter");
  ADD_METHOD_TO(PairingController::status, "/pairing/status", drogon::Get,
                "DeviceFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> pair(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);

private:
  PairingFeatureService service_;
};
