#pragma once

#include <shared/services/tts/tts-wire.hxx>

#include <cstdint>
#include <string>
#include <vector>

// Remote TTS endpoint settings: when tts.remote_url is set, every synthesis
// is an HTTP call to argus-tts (:7029) and a down service surfaces as an
// exception, never as an in-process fallback.
struct TtsRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  // Reads tts.remote_url / tts.remote_timeout_ms from the loaded config.
  static TtsRemoteConfig resolve();
};

// One internal-wire exchange: method + path + optional JSON body; the stream
// leg keeps the connection open, so it opts out of Connection: close.
struct WireRequest
{
  std::string method;
  std::string path;
  std::string body;
  bool closeConnection{true};
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

  RawResponse exchange(const WireRequest& request) const;
  void stream(const WireRequest& request,
              const std::function<void(const char*, size_t)>& onChunk) const;

  std::string baseUrl_;
  int timeoutMs_;
};

// The TTS entry point consumers hold as a member: every call is an HTTP
// exchange with argus-tts and throws std::runtime_error when tts.remote_url
// is not configured — there is no in-process fallback.
class TtsClient
{
public:
  float defaultSpeed() const;
  int sampleRate() const;
  std::vector<float> synthesize(const TtsRequest& req) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;
  bool remote() const;
};
