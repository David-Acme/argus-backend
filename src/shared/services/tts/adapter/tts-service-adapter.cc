#include "tts-service-adapter.hxx"

#include <shared/services/tts/tts-service.hxx>

bool TtsServiceAdapter::initialize()
{
  ttsService_.init();
  return ttsService_.isLoaded();
}

bool TtsServiceAdapter::isLoaded() const
{
  return ttsService_.isLoaded();
}

void TtsServiceAdapter::shutdown()
{
  ttsService_.shutdown();
}

Json::Value TtsServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = ttsService_.isLoaded();
  return value;
}
