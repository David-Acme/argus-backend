#pragma once

#include <config/service.hxx>
#include <memory>
#include <shared/services/mdns/mdns-service.hxx>

class MdnsServiceAdapter : public IService
{
public:
  std::string name() const override { return "mdns"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

private:
  std::unique_ptr<MdnsService> service_;
};
