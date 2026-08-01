#include "service-registry.hxx"

#include <algorithm>
#include <atomic>
#include <drogon/drogon.h>
#include <thread>
#include <vector>

void ServiceRegistry::registerService(std::unique_ptr<IService> service)
{
  services_.push_back(std::move(service));
}

bool ServiceRegistry::initialize()
{
  for (const auto& service : services_) {
    for (const auto& dep : service->dependencies()) {
      const auto found =
          std::find_if(services_.begin(), services_.end(),
                       [&](const auto& s) { return s->name() == dep; });
      if (found == services_.end())
        LOG_WARN << "Service " << service->name() << " depends on missing "
                 << dep;
    }
  }

  std::atomic<bool> ok{true};
  std::vector<std::thread> threads;
  threads.reserve(services_.size());
  for (const auto& service : services_) {
    LOG_INFO << "Initializing service: " << service->name() << " v"
             << service->version();
    threads.emplace_back([&ok, &service] {
      if (!service->initialize()) {
        LOG_FATAL << "Service " << service->name()
                  << " failed to initialize";
        ok.store(false);
      }
    });
  }

  for (auto& thread : threads)
    thread.join();
  return ok.load();
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
