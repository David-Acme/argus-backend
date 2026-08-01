#pragma once

#include <config/service-registry.hxx>

class Application
{
public:
  int run();
  void shutdown();

private:
  void registerServices();

  ServiceRegistry registry_;
};
