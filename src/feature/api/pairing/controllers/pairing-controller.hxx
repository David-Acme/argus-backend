#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class PairingController : public drogon::HttpController<PairingController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(PairingController::pair, "/pairing", drogon::Post,
                "ValidJsonFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> pair(drogon::HttpRequestPtr req);
};
