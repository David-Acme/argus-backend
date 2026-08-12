#include "llm-service-adapter.hxx"

#include <shared/services/llm/llm-service.hxx>

bool LlmServiceAdapter::initialize()
{
  LlmService::instance().init();
  return LlmService::instance().isLoaded();
}

bool LlmServiceAdapter::isLoaded() const
{
  return LlmService::instance().isLoaded();
}

void LlmServiceAdapter::shutdown()
{
  LlmService::instance().shutdown();
}

Json::Value LlmServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = LlmService::instance().isLoaded();
  return value;
}
