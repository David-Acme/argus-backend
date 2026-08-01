#include "llm-service-adapter.hxx"

#include <shared/services/llm/llm-service.hxx>

bool LlmServiceAdapter::initialize()
{
  LlmService::init();
  return LlmService::isLoaded();
}

bool LlmServiceAdapter::isLoaded() const
{
  return LlmService::isLoaded();
}

void LlmServiceAdapter::shutdown()
{
  LlmService::shutdown();
}

Json::Value LlmServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = LlmService::isLoaded();
  return value;
}
