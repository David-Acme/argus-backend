#include "stt-service-adapter.hxx"

#include <shared/services/stt/stt-service.hxx>

bool SttServiceAdapter::initialize()
{
  SttService::init();
  return SttService::isLoaded();
}

bool SttServiceAdapter::isLoaded() const
{
  return SttService::isLoaded();
}

void SttServiceAdapter::shutdown()
{
  SttService::shutdown();
}

Json::Value SttServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = SttService::isLoaded();
  return value;
}
