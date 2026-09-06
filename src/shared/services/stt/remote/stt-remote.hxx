#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The internal wire contract (Ruling BL): 16 kHz mono s16 PCM, with the
// float side scaled by /32768 exactly like the voice session's own WS-frame
// conversion, so A/B text equality holds up to quantization.
inline constexpr int32_t kWireSampleRate = 16000;
inline constexpr float kPcmScale = 32768.0F;

// Cutover plumbing for the STT engine (Rulings BM/BN): the legacy voice
// session transcribes in-process until stt.remote_url is configured; from
// then on every turn is an HTTP call to argus-stt (:7030) and a down service
// surfaces as an exception, never as an in-process fallback.
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

// HTTP client for the argus-stt internal wire (Ruling BL): the transcribe
// endpoint (binary s16 PCM in, frozen-envelope JSON out). Throws
// std::runtime_error carrying the frozen envelope error on failure.
class SttHttpClient
{
public:
  SttHttpClient(std::string baseUrl, int timeoutMs);

  // 16 kHz mono float samples in (the voice session's format), transcribed
  // text out. `lang` rides the query string; "" resolves server-side from
  // the service's stt.language.
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
