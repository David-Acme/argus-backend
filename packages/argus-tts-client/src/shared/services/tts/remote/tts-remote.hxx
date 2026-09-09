#pragma once

#include <shared/services/tts/tts-wire.hxx>

#include <cstdint>
#include <string>
#include <vector>

// Remote TTS endpoint settings; every synthesis is an HTTP call once tts.remote_url is set.
struct TtsRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  // Reads tts.remote_url / tts.remote_timeout_ms from the loaded config.
  static TtsRemoteConfig resolve();
};

// One internal-wire exchange; the stream leg keeps the connection open.
struct WireRequest
{
  std::string method;
  std::string path;
  std::string body;
  bool closeConnection{true};
};

// HTTP client for the argus-tts internal wire; throws std::runtime_error with the frozen envelope error.
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

// TTS entry point held by consumers; throws when tts.remote_url is not configured.
class TtsClient
{
public:
  float defaultSpeed() const;
  int sampleRate() const;
  std::vector<float> synthesize(const TtsRequest& req) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;
  bool remote() const;
};
