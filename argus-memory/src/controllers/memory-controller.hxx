#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <memory/memory-dto.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/llm/remote/llm-remote.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/remote/wire-memory-chat.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/tools/tool-registry.hxx>

// Owns the memory stack by value; no singleton.
class MemoryController
    : public drogon::HttpController<MemoryController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(MemoryController::remember, "/memory/v1/remember",
                drogon::Post);
  ADD_METHOD_TO(MemoryController::recall, "/memory/v1/recall", drogon::Post);
  ADD_METHOD_TO(MemoryController::forget, "/memory/v1/forget", drogon::Post);
  ADD_METHOD_TO(MemoryController::procedureRun, "/memory/v1/procedure-run",
                drogon::Post);
  ADD_METHOD_TO(MemoryController::capture, "/memory/v1/capture", drogon::Post);
  ADD_METHOD_TO(MemoryController::compact, "/memory/v1/compact", drogon::Post);
  ADD_METHOD_TO(MemoryController::durableTranscript,
                "/memory/v1/durable-transcript", drogon::Post);
  METHOD_LIST_END

  // Boots the memory stack (deferStore) and registers the tool handlers.
  MemoryController();

  void initStack();
  void shutdownStack();
  bool isStackLoaded() const;

  MemoryService& service() { return service_; }
  ToolRegistry& registry() { return registry_; }

  drogon::Task<drogon::HttpResponsePtr> remember(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> recall(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> forget(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> procedureRun(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> capture(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> compact(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> durableTranscript(
      drogon::HttpRequestPtr req);

private:
  drogon::Task<drogon::HttpResponsePtr>
  runTool(const std::string& name, const Json::Value& arguments,
          const MemoryToolContext& context);

  // Worker chats ride the argus-llm wire.
  std::unique_ptr<WireMemoryChat> chat_;
  MemoryService service_;
  ToolRegistry registry_;
};
