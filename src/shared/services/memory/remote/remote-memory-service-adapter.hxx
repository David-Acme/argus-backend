#pragma once

#include <config/service.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/remote/memory-remote.hxx>

#include <json/value.h>
#include <memory>
#include <string>

// Remote stand-in for MemoryServiceAdapter (Ruling BY): same IService name
// and the same tool descriptors registered into the tool registry, but every
// handler serves from argus-memory over the internal wire. When
// memory.remote_url is set the legacy boots WITHOUT the memory stack — no
// sqlite-vec, no NuExtract, no embedding models ever load here.
class RemoteMemoryServiceAdapter : public IService
{
public:
  std::string name() const override { return "memory"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  // The call surface beyond the tool loop, mirroring MemoryService's own
  // methods so the consumers compile against either substrate.
  CaptureResult captureExplicit(const CaptureInput& input);
  void enqueueCompaction(int64_t userId, const std::string& transcript,
                         const std::string& lang);
  std::string durableTranscript(const std::string& transcript,
                                const std::string& lang);

private:
  MemoryRemoteConfig config_;
  std::unique_ptr<MemoryHttpClient> client_;
};
