#include "tts-service-adapter.hxx"

#include <shared/services/tts/tts-service.hxx>

bool TtsServiceAdapter::initialize()
{
  TtsService::init();
  return TtsService::isLoaded();
}

bool TtsServiceAdapter::isLoaded() const
{
  return TtsService::isLoaded();
}

void TtsServiceAdapter::shutdown()
{
  TtsService::shutdown();
}

Json::Value TtsServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = TtsService::isLoaded();
  return value;
}
