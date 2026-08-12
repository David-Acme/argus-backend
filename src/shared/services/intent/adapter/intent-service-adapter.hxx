#pragma once

#include <config/service.hxx>
#include <shared/services/intent/intent-service.hxx>

class IntentServiceAdapter : public IService
{
public:
  std::string name() const override { return "intent"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

private:
  IntentService intentService_;
};
