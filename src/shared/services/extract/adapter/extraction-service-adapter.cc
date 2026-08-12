#include <json/value.h>
#include <shared/services/extract/adapter/extraction-service-adapter.hxx>

bool ExtractionServiceAdapter::initialize()
{
  return true;
}

bool ExtractionServiceAdapter::isLoaded() const
{
  return service_.isLoaded();
}

void ExtractionServiceAdapter::shutdown()
{
  service_.unloadIfIdle();
}

Json::Value ExtractionServiceAdapter::health() const
{
  Json::Value root;
  root["loaded"] = service_.isLoaded();
  return root;
}
