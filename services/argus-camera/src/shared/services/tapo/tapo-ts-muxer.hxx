#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TapoTsConfig
{
  uint16_t pmtPid{0x1000};
  uint16_t audioPid{0x0100};
  uint16_t programNumber{1};
  uint8_t streamType{0x90};
  uint8_t streamId{0xC0};
};

struct TapoTsFrame
{
  std::vector<uint8_t> payload;
  int64_t pts90k{0};
};

class TapoTsMuxer
{
public:
  explicit TapoTsMuxer(TapoTsConfig config);

  std::string tables();
  std::string frame(const TapoTsFrame& input);
  void reset();

private:
  std::vector<uint8_t> section(uint8_t tableId, const std::vector<uint8_t>& body);

  struct PacketizeInput
  {
    uint16_t pid{0};
    const std::vector<uint8_t>& payload;
    bool sectionPayload{false};
    bool withPcr{false};
    int64_t pcr90k{0};
  };

  std::string packetize(const PacketizeInput& input);

  TapoTsConfig config_;
  uint8_t patCounter_{0};
  uint8_t pmtCounter_{0};
  uint8_t audioCounter_{0};
};
