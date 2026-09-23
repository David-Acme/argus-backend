#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

inline constexpr int32_t kWireSampleRate = 16000;
inline constexpr float kPcmScale = 32768.0F;

struct SttRemoteConfig
{
  std::string url;
  int timeoutMs{30000};

  bool enabled() const { return !url.empty(); }

  static SttRemoteConfig resolve();
};

struct SttWireRequest
{
  std::string path;
  std::string body;
  std::string contentType;
};

class SttHttpClient
{
public:
  SttHttpClient(std::string baseUrl, int timeoutMs);

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
