#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/llm/services/intent-gate.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <llm/llm-service.hxx>

class LlmController : public drogon::HttpController<LlmController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(LlmController::chat, "/llm/v1/chat", drogon::Post);
  ADD_METHOD_TO(LlmController::chatStream, "/llm/v1/chat-stream", drogon::Post);
  ADD_METHOD_TO(LlmController::engine, "/llm/v1/config", drogon::Get);
  METHOD_LIST_END

  LlmController() : adapter_(service_, &intentGate_.router()) {}

  void initEngine();
  void shutdownEngine();
  bool isEngineLoaded();

  LlmService& service() { return service_; }

  LfmAdapter& adapter() { return adapter_; }

  const IntentRouter& router() const { return intentGate_.router(); }

  drogon::Task<drogon::HttpResponsePtr> chat(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> chatStream(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);

private:
  LlmService service_;
  IntentGate intentGate_;
  LfmAdapter adapter_;
};
