#include "stt-service-adapter.hxx"

#include <shared/services/stt/stt-service.hxx>

bool SttServiceAdapter::initialize()
{
  SttService::instance().init();
  return SttService::instance().isLoaded();
}

bool SttServiceAdapter::isLoaded() const
{
  return SttService::instance().isLoaded();
}

void SttServiceAdapter::shutdown()
{
  SttService::instance().shutdown();
}

Json::Value SttServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = SttService::instance().isLoaded();
  return value;
}
