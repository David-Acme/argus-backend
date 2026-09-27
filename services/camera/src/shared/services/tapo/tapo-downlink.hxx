#pragma once

#include <cstdint>
#include <vector>

enum class TapoDownlinkCodec : uint8_t
{
  None = 0,
  ALaw,
  ULaw
};

struct TapoDownlinkChunk
{
  std::vector<int16_t> samples;
  int sampleRate{8000};
  TapoDownlinkCodec codec{TapoDownlinkCodec::None};
};

class TapoDownlink
{
public:
  void feed(const std::vector<uint8_t>& bytes);
  bool take(TapoDownlinkChunk& chunk);
  void reset();

  [[nodiscard]] TapoDownlinkCodec codec() const;

private:
  void parsePacket(const uint8_t* packet);
  void parsePat(const uint8_t* payload);
  void parsePmt(const uint8_t* payload);
  void appendPes(const uint8_t* payload, size_t size, bool start);
  void flushPes();

  std::vector<uint8_t> buffer_;
  uint16_t pmtPid_{0};
  uint16_t audioPid_{0};
  uint8_t audioStreamType_{0};
  std::vector<uint8_t> pesPayload_;
  int64_t pesRemaining_{0};
  std::vector<int16_t> pending_;
  int sampleRate_{8000};
};
