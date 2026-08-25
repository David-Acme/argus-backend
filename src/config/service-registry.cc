#include "service-registry.hxx"

#include <drogon/drogon.h>
#include <unordered_map>
#include <vector>

void ServiceRegistry::registerService(std::unique_ptr<IService> service)
{
  services_.push_back(std::move(service));
}

bool ServiceRegistry::initialize()
{
  std::unordered_map<std::string, size_t> indexes;
  indexes.reserve(services_.size());
  for (size_t i = 0; i < services_.size(); ++i) {
    if (!indexes.emplace(services_[i]->name(), i).second) {
      LOG_ERROR << "Duplicate service registration: " << services_[i]->name();
      return false;
    }
  }

  std::vector<bool> initialized(services_.size(), false);
  for (const auto& service : services_) {
    for (const auto& dep : service->dependencies()) {
      if (!indexes.contains(dep)) {
        LOG_ERROR << "Service " << service->name()
                  << " depends on missing " << dep;
        return false;
      }
    }
  }

  // Initialize in dependency order. Services acquire shared resources during
  // initialize(), so concurrent startup makes dependency declarations
  // advisory and introduces races between DB, queue, and socket services.
  size_t initializedCount = 0;
  while (initializedCount < services_.size()) {
    bool progressed = false;
    for (size_t i = 0; i < services_.size(); ++i) {
      if (initialized[i])
        continue;

      bool dependenciesReady = true;
      for (const auto& dep : services_[i]->dependencies()) {
        if (!initialized[indexes.at(dep)]) {
          dependenciesReady = false;
          break;
        }
      }
      if (!dependenciesReady)
        continue;

      const auto& service = services_[i];
      LOG_INFO << "Initializing service: " << service->name() << " v"
               << service->version();
      if (!service->initialize()) {
        LOG_FATAL << "Service " << service->name()
                  << " failed to initialize";
        return false;
      }
      initialized[i] = true;
      ++initializedCount;
      progressed = true;
    }

    if (!progressed) {
      LOG_ERROR << "Service dependency cycle detected";
      return false;
    }
  }

  return true;
}

void ServiceRegistry::shutdownAll()
{
  for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
    LOG_INFO << "Shutting down service: " << (*it)->name();
    (*it)->shutdown();
  }
}

Json::Value ServiceRegistry::health() const
{
  Json::Value root(Json::objectValue);
  for (const auto& service : services_) {
    auto entry = service->health();
    entry["version"] = service->version();
    root[service->name()] = entry;
  }
  return root;
}

std::vector<std::string> ServiceRegistry::names() const
{
  std::vector<std::string> result;
  result.reserve(services_.size());
  for (const auto& service : services_)
    result.push_back(service->name());
  return result;
}
