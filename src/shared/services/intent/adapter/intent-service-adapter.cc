#include "intent-service-adapter.hxx"

#include <shared/services/intent/intent-service.hxx>

bool IntentServiceAdapter::initialize()
{
  intentService_.init();
  return true;
}

bool IntentServiceAdapter::isLoaded() const
{
  return intentService_.isLoaded();
}

void IntentServiceAdapter::shutdown()
{
  intentService_.shutdown();
}

Json::Value IntentServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = intentService_.isLoaded();
  return value;
}
