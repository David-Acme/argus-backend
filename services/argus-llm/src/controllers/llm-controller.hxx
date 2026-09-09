#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <shared/services/llm/intent-gate.hxx>
#include <shared/services/llm/llm-service.hxx>

// Owns the LLM engine by value; no singleton.
class LlmController : public drogon::HttpController<LlmController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(LlmController::chat, "/llm/v1/chat", drogon::Post);
  ADD_METHOD_TO(LlmController::chatStream, "/llm/v1/chat-stream", drogon::Post);
  ADD_METHOD_TO(LlmController::engine, "/llm/v1/config", drogon::Get);
  METHOD_LIST_END

  void initEngine();
  void shutdownEngine();
  bool isEngineLoaded();

  LlmService& service() { return service_; }

  // The fast tier in front of the tool loop. Always non-null: an unloaded
  // model makes the router abstain, it does not remove it.
  const IntentRouter& router() const { return intentGate_.router(); }

  drogon::Task<drogon::HttpResponsePtr> chat(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> chatStream(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);

private:
  LlmService service_;
  IntentGate intentGate_;
};
