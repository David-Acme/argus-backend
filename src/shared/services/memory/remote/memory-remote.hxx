#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// Memory cutover plumbing: the workers serve from argus-memory once memory.remote_url is set.
struct MemoryRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads memory.remote_url / memory.remote_timeout_ms from the loaded config.
  static MemoryRemoteConfig resolve();
};

// HTTP client for the argus-memory internal wire; throws on failure.
class MemoryHttpClient
{
public:
  MemoryHttpClient(std::string baseUrl, int timeoutMs);

  // Returns the response's info object; throws on non-200 or a malformed envelope.
  Json::Value call(const std::string& path, const Json::Value& body) const;

private:
  std::string baseUrl_;
  int timeoutMs_;
};
