#pragma once

#include <cstdint>
#include <string>

// Cutover plumbing for the vision engine (Ruling BQ): the legacy boots its
// in-process VisionService until vision.remote_url is configured; from then
// on describeMat is an HTTP call to argus-vlm (:7031) and the model is never
// loaded in the legacy process.
struct VlmRemoteConfig
{
  std::string url;
  int timeoutMs{120000};

  bool enabled() const { return !url.empty(); }

  // Reads vision.remote_url / vision.remote_timeout_ms from the loaded
  // config.
  static VlmRemoteConfig resolve();
};

// One internal-wire exchange: JSON body + its MIME (the describe wire is
// JSON in and JSON out, unlike the binary TTS/STT wires).
struct VlmWireRequest
{
  std::string path;
  std::string body;
  std::string contentType;
};

// Parameter struct for one describe call (AGENTS rule 2).
struct VlmDescribeInput
{
  std::string imageJpegB64;
  // Empty prompt/camera_id stay off the wire body (optional fields).
  std::string prompt;
  std::string cameraId;
};

// HTTP client for the argus-vlm internal wire (Ruling BP): the describe
// endpoint (base64 JPEG + optional prompt/camera_id in, frozen-envelope JSON
// out). The caller encodes the cv::Mat to JPEG first — the in-process API
// takes a Mat, the wire takes encoded bytes. Throws std::runtime_error
// carrying the frozen envelope error on failure.
class VlmHttpClient
{
public:
  VlmHttpClient(std::string baseUrl, int timeoutMs);

  // Base64 JPEG in, caption text out. prompt/camera_id ride the body only
  // when non-empty (the wire contract marks them optional).
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
