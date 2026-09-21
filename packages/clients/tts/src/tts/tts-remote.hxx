#pragma once

#include <tts/tts-wire.hxx>

#include <atomic>
#include <memory>
#include <cstdint>
#include <string>
#include <stop_token>
#include <vector>

namespace argus::tts { class Client; }

struct TtsRemoteStreamInput
{
  TtsRequest request;
  TtsChunkCallback onChunk;
  std::stop_token cancellation;
};

struct TtsHttpStreamInput
{
  std::function<void(const char*, size_t)> onChunk;
  std::stop_token cancellation;
};

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

  float defaultSpeed(std::stop_token cancellation = {}) const;
  int sampleRate(std::stop_token cancellation = {}) const;
  std::vector<float> synthesize(const TtsRequest& req,
                                 std::stop_token cancellation = {}) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk,
                         std::stop_token cancellation) const;
  void synthesizeStream(TtsRemoteStreamInput input) const;

private:
  struct RawResponse
  {
    int status{0};
    std::string body;
  };

  RawResponse exchange(const WireRequest& request,
                       std::stop_token cancellation = {}) const;
  void stream(const WireRequest& request, const TtsHttpStreamInput& input) const;

  std::string baseUrl_;
  int timeoutMs_;
};

// TTS entry point held by consumers; throws when tts.remote_url is not configured.
class TtsClient
{
public:
  float defaultSpeed(std::stop_token cancellation = {}) const;
  int sampleRate(std::stop_token cancellation = {}) const;
  std::vector<float> synthesize(const TtsRequest& req,
                                 std::stop_token cancellation = {}) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) const;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk,
                         std::stop_token cancellation) const;
  void synthesizeStream(TtsRemoteStreamInput input) const;
  bool remote() const;

private:
  std::shared_ptr<argus::tts::Client> rpcClient() const;
  mutable std::atomic<std::shared_ptr<argus::tts::Client>> rpcClient_;
};
