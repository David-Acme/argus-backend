#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// Cutover plumbing for the memory stack (Ruling BY): the legacy workers
// serve memory from argus-memory (:7033) once memory.remote_url is
// configured; the in-process stack stays dormant.
struct MemoryRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads memory.remote_url / memory.remote_timeout_ms from the loaded
  // config.
  static MemoryRemoteConfig resolve();
};

// HTTP client for the argus-memory internal wire: one POST per tool call,
// frozen {status, info, errors} envelope in, info object out. Throws
// std::runtime_error carrying the envelope error on failure.
class MemoryHttpClient
{
public:
  MemoryHttpClient(std::string baseUrl, int timeoutMs);

  // Returns the response's info object; throws on non-200 or a malformed
  // envelope.
  Json::Value call(const std::string& path, const Json::Value& body) const;

private:
  std::string baseUrl_;
  int timeoutMs_;
};
