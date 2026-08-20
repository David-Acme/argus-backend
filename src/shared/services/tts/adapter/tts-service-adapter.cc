#include "tts-service-adapter.hxx"

#include <shared/services/tts/tts-service.hxx>

bool TtsServiceAdapter::initialize()
{
  TtsService::instance().init();
  return TtsService::instance().isLoaded();
}

bool TtsServiceAdapter::isLoaded() const
{
  return TtsService::instance().isLoaded();
}

void TtsServiceAdapter::shutdown()
{
  TtsService::instance().shutdown();
}

Json::Value TtsServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = TtsService::instance().isLoaded();
  return value;
}
