#include "memory-service-adapter.hxx"

#include <shared/services/tools/tool-registry.hxx>

bool MemoryServiceAdapter::initialize()
{
  memoryService_.init({.deferStore = true});
  memoryService_.registerTools(ToolRegistry::instance());
  return true;
}

bool MemoryServiceAdapter::isLoaded() const
{
  return memoryService_.isLoaded();
}

void MemoryServiceAdapter::shutdown()
{
  memoryService_.shutdown();
}

Json::Value MemoryServiceAdapter::health() const
{
  Json::Value value(Json::objectValue);
  value["loaded"] = memoryService_.isLoaded();
  return value;
}
