#include "vision-service-adapter.hxx"

#include <shared/services/vision/vision-service.hxx>

bool VisionServiceAdapter::initialize()
{
  VisionService::init();
  return VisionService::isLoaded();
}

bool VisionServiceAdapter::isLoaded() const
{
  return VisionService::isLoaded();
}

void VisionServiceAdapter::shutdown()
{
  VisionService::shutdown();
}

Json::Value VisionServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = VisionService::isLoaded();
  return value;
}
