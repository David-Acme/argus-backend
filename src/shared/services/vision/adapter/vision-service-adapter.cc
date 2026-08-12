#include "vision-service-adapter.hxx"

#include <shared/services/vision/vision-service.hxx>

bool VisionServiceAdapter::initialize()
{
  visionService_.init();
  return visionService_.isLoaded();
}

bool VisionServiceAdapter::isLoaded() const
{
  return visionService_.isLoaded();
}

void VisionServiceAdapter::shutdown()
{
  visionService_.shutdown();
}

Json::Value VisionServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = visionService_.isLoaded();
  return value;
}
