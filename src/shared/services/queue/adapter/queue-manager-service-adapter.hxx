#pragma once

#include <config/service.hxx>
#include <json/value.h>
#include <shared/services/queue/job-queue.hxx>

class QueueManagerServiceAdapter : public IService
{
public:
  std::string name() const override { return "queue"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  QueueManager& manager() { return manager_; }

private:
  QueueManager manager_;
};
