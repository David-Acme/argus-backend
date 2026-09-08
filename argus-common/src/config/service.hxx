#pragma once

#include <json/value.h>
#include <string>
#include <vector>

class IService
{
public:
  virtual ~IService() = default;

  virtual std::string name() const = 0;
  virtual std::string version() const = 0;
  virtual std::vector<std::string> dependencies() const { return {}; }

  virtual bool initialize() = 0;
  virtual bool isLoaded() const = 0;
  virtual void shutdown() = 0;
  virtual Json::Value health() const = 0;
};
