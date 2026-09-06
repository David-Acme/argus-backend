#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class SttController : public drogon::HttpController<SttController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(SttController::transcribe, "/stt/v1/transcribe", drogon::Post);
  ADD_METHOD_TO(SttController::engine, "/stt/v1/config", drogon::Get);
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr>
  transcribe(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);
};
