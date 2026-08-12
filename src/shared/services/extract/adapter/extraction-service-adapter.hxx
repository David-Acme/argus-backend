#pragma once

#include <config/service.hxx>
#include <shared/services/extract/extraction-service.hxx>

class ExtractionServiceAdapter : public IService
{
public:
  std::string name() const override { return "extract"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  ExtractionService& service() { return service_; }

private:
  ExtractionService service_;
};
