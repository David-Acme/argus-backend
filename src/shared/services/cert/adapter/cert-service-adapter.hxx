#pragma once

#include <config/service.hxx>
#include <json/value.h>
#include <string>

class CertServiceAdapter : public IService
{
public:
  std::string name() const override { return "cert"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;
};
