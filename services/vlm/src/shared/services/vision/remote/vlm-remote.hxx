#pragma once

#include <cstdint>
#include <string>

struct VlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  static VlmRemoteConfig resolve();
};

struct VlmWireRequest
{
  std::string path;
  std::string body;
  std::string contentType;
};

struct VlmDescribeInput
{
  std::string imageJpegB64;
  std::string prompt;
  std::string cameraId;
};

class VlmHttpClient
{
public:
  VlmHttpClient(std::string baseUrl, int timeoutMs);

  std::string describe(const VlmDescribeInput& input) const;

private:
  struct RawResponse
  {
    int status{0};
    std::string body;
  };

  RawResponse exchange(const VlmWireRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
};
