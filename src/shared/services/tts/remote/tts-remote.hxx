#pragma once

#include <shared/services/tts/tts-service.hxx>

#include <cstdint>
#include <string>
#include <vector>

// Cutover plumbing for the TTS engine (Rulings BI/BJ): legacy consumers call
// the in-process TtsService singleton until tts.remote_url is configured;
// from then on every synthesis is an HTTP call to argus-tts (:7029) and a
// down service surfaces as an exception, never as an in-process fallback.
struct TtsRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  // Reads tts.remote_url / tts.remote_timeout_ms from the loaded config.
  static TtsRemoteConfig resolve();
};

// HTTP client for the argus-tts internal wire (Ruling BH): the two
// synthesize endpoints plus GET /tts/v1/config for defaultSpeed/sampleRate.
// Throws std::runtime_error carrying the frozen envelope error on failure.
class TtsHttpClient
{
public:
  TtsHttpClient(std::string baseUrl, int timeoutMs);

  float defaultSpeed() const;
  int sampleRate() const;
  std::vector<float> synthesize(const TtsRequest& req) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;

private:
  struct RawResponse
  {
    int status{0};
    std::string body;
  };

  RawResponse exchange(const std::string& method, const std::string& path,
                       const std::string& body) const;
  void stream(const std::string& method, const std::string& path,
              const std::string& body,
              const std::function<void(const char*, size_t)>& onChunk) const;

  std::string baseUrl_;
  int timeoutMs_;
};

// The TTS entry point legacy consumers hold as a member: dispatches every
// call to argus-tts when the cutover is configured, else to the in-process
// singleton. Resolution happens per call, so tests can flip it at runtime.
class TtsClient
{
public:
  float defaultSpeed() const;
  int sampleRate() const;
  std::vector<float> synthesize(const TtsRequest& req) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;
  bool remote() const;
};
