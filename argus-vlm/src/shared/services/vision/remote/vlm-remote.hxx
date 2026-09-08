#pragma once

#include <cstdint>
#include <string>

// Vision cutover plumbing: describe goes over the argus-vlm wire once vision.remote_url is set.
struct VlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads vision.remote_url / vision.remote_timeout_ms from the loaded config.
  static VlmRemoteConfig resolve();
};

// One internal-wire exchange: JSON body + its MIME.
struct VlmWireRequest
{
  std::string path;
  std::string body;
  std::string contentType;
};

// One describe call; empty prompt/camera_id stay off the wire body.
struct VlmDescribeInput
{
  std::string imageJpegB64;
  std::string prompt;
  std::string cameraId;
};

// HTTP client for the argus-vlm internal wire; throws std::runtime_error with the frozen envelope error.
class VlmHttpClient
{
public:
  VlmHttpClient(std::string baseUrl, int timeoutMs);

  // Base64 JPEG in, caption text out.
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
