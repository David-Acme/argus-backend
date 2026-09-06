#pragma once

#include <config/service.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-chat.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>

class MemoryServiceAdapter : public IService
{
public:
  std::string name() const override { return "memory"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

private:
  InProcessMemoryChat chat_{LlmService::instance()};
  MemoryService memoryService_{VecDb::instance(), chat_};
};
