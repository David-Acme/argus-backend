#include "stt-service-adapter.hxx"

#include <shared/services/stt/stt-service.hxx>

bool SttServiceAdapter::initialize()
{
  sttService_.init();
  return sttService_.isLoaded();
}

bool SttServiceAdapter::isLoaded() const
{
  return sttService_.isLoaded();
}

void SttServiceAdapter::shutdown()
{
  sttService_.shutdown();
}

Json::Value SttServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = sttService_.isLoaded();
  return value;
}
