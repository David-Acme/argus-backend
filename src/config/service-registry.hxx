#pragma once

#include <config/service.hxx>
#include <json/value.h>
#include <memory>
#include <string>
#include <vector>

class ServiceRegistry
{
public:
  void registerService(std::unique_ptr<IService> service);

  bool initialize();
  void shutdownAll();
  Json::Value health() const;
  std::vector<std::string> names() const;

private:
  std::vector<std::unique_ptr<IService>> services_;
};
