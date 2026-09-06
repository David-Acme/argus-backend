#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

class TtsController : public drogon::HttpController<TtsController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(TtsController::synthesize, "/tts/v1/synthesize", drogon::Post,
                "ValidJsonFilter");
  ADD_METHOD_TO(TtsController::synthesizeStream, "/tts/v1/synthesize-stream",
                drogon::Post, "ValidJsonFilter");
  ADD_METHOD_TO(TtsController::engine, "/tts/v1/config", drogon::Get);
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr>
  synthesize(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  synthesizeStream(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);
};
