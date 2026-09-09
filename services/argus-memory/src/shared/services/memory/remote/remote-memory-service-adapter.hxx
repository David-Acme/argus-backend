#pragma once

#include <config/service.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/remote/memory-remote.hxx>

#include <json/value.h>
#include <memory>
#include <string>

// Remote stand-in for MemoryServiceAdapter: every handler serves from argus-memory over the wire.
class RemoteMemoryServiceAdapter : public IService
{
public:
  std::string name() const override { return "memory"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  // Call surface beyond the tool loop, mirroring MemoryService's own methods.
  CaptureResult captureExplicit(const CaptureInput& input);
  void enqueueCompaction(int64_t userId, const std::string& transcript,
                         const std::string& lang);
  std::string durableTranscript(const std::string& transcript,
                                const std::string& lang);

private:
  MemoryRemoteConfig config_;
  std::unique_ptr<MemoryHttpClient> client_;
};
