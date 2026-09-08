#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Internal wire contract: 16 kHz mono s16 PCM, float scaled by /32768 like the voice session's WS frames.
inline constexpr int32_t kWireSampleRate = 16000;
inline constexpr float kPcmScale = 32768.0F;

// STT cutover plumbing: every turn is an HTTP call to argus-stt once stt.remote_url is set.
struct SttRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  // Reads stt.remote_url / stt.remote_timeout_ms from the loaded config.
  static SttRemoteConfig resolve();
};

// One internal-wire exchange: path (with query) + binary body + its MIME.
struct SttWireRequest
{
  std::string path;
  std::string body;
  std::string contentType;
};

// HTTP client for the argus-stt internal wire; throws std::runtime_error with the frozen envelope error.
class SttHttpClient
{
public:
  SttHttpClient(std::string baseUrl, int timeoutMs);

  // Transcribes 16 kHz mono float samples; `lang` rides the query string, "" resolves server-side.
  std::string transcribe(const std::vector<float>& audioSamples,
                         const std::string& lang) const;

private:
  struct RawResponse
  {
    int status{0};
    std::string body;
  };

  RawResponse exchange(const SttWireRequest& request) const;

  std::string baseUrl_;
  int timeoutMs_;
};
