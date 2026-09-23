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

struct TtsRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  static TtsRemoteConfig resolve();
};

struct WireRequest
{
  std::string method;
  std::string path;
  std::string body;
  bool closeConnection{true};
};

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
