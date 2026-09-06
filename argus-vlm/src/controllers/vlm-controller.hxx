#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <shared/services/vision/vision-service.hxx>

// Owns the vision engine by value (the VisionServiceAdapter shape: no
// singleton). The engine is THE capacity of this service.
class VlmController : public drogon::HttpController<VlmController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(VlmController::describe, "/vlm/v1/describe", drogon::Post);
  ADD_METHOD_TO(VlmController::engine, "/vlm/v1/config", drogon::Get);
  METHOD_LIST_END

  void initEngine();
  void shutdownEngine();
  bool isEngineLoaded() const;

  VisionService& service() { return service_; }

  drogon::Task<drogon::HttpResponsePtr> describe(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);

private:
  VisionService service_;
};
