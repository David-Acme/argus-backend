#pragma once

#include <cstdint>
#include <memory>
#include <shared/services/tapo/tapo-http.hxx>
#include <shared/services/tapo/tapo-transport.hxx>
#include <shared/services/tapo/tapo-ts-muxer.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <string>
#include <vector>

enum class TapoTalkFraming : uint8_t
{
  None = 0,
  NegativeLength,
  Chunked
};

inline TapoTalkFraming tapoTalkFramingFromString(const std::string& value)
{
  if (value == "negative")
    return TapoTalkFraming::NegativeLength;
  if (value == "chunked")
    return TapoTalkFraming::Chunked;
  return TapoTalkFraming::None;
}

inline std::string tapoTalkFramingToString(TapoTalkFraming framing)
{
  switch (framing) {
    case TapoTalkFraming::NegativeLength:
      return "negative";
    case TapoTalkFraming::Chunked:
      return "chunked";
    case TapoTalkFraming::None:
      return "none";
  }
  return "none";
}

struct TapoTalkConfig
{
  std::string host;
  int port{8800};
  std::string username{"admin"};
  std::string cloudPassword;
  std::string mode{"aec"};
  int connectTimeoutMs{3000};
  int ioTimeoutMs{5000};
  int packetMs{20};
  bool pace{true};
  TapoTalkFraming framing{TapoTalkFraming::None};
  TapoTsConfig ts;
};

struct TapoTalkAudio
{
  std::vector<int16_t> samples;
  int sampleRate{8000};
};

struct TapoTalkSendInput
{
  std::vector<int16_t> samples;
  int sampleRate{8000};
  bool reopenOnFailure{false};
};

struct TapoSpeakerGainInput
{
  std::vector<int16_t> samples;
  double maxGain{3.0};
  double targetPeak{26000.0};
};

std::vector<int16_t> tapoApplySpeakerGain(const TapoSpeakerGainInput& input);

struct TapoTalkPart
{
  std::vector<TapoHttpHeader> headers;
  std::string body;

  std::string header(const std::string& name) const;
};

class TapoTalkClient
{
public:
  explicit TapoTalkClient(TapoTalkConfig config);
  ~TapoTalkClient();

  TapoTalkClient(const TapoTalkClient&) = delete;
  TapoTalkClient& operator=(const TapoTalkClient&) = delete;

  TapoResult open();
  TapoResult send(const TapoTalkAudio& audio, const CancellationToken& token);
  TapoResult sendChunk(const TapoTalkSendInput& input,
                       const CancellationToken& token);
  void close();

  bool isOpen() const;
  // Duration of audio actually written to the talk channel, in milliseconds.
  int64_t sentDurationMs() const;
  Json::Value state() const;

private:
  std::string requestHead(const std::string& authorization) const;
  TapoResult handshake(const std::string& authorization,
                       TapoHttpResponse& response);
  TapoResult authenticate();
  TapoResult startSession();
  bool writePart(const std::vector<TapoHttpHeader>& headers,
                 const std::string& body);
  bool readPart(TapoTalkPart& part);

  TapoTalkConfig config_;
  std::unique_ptr<TapoConnection> connection_;
  TapoTsMuxer muxer_;
  std::string passwordVariant_;
  std::string sessionId_;
  std::string keyExchangeNonce_;
  int64_t seq_{1};
  int64_t pts90k_{0};
  int64_t sentSamples_{0};
  bool open_{false};
};
