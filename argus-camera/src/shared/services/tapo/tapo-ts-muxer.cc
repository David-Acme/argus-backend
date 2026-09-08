#include "tapo-ts-muxer.hxx"

#include <algorithm>
#include <utility>

namespace
{

constexpr size_t kPacketSize = 188;
constexpr uint16_t kPatPid = 0x0000;

uint32_t crc32Mpeg(const std::vector<uint8_t>& data)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (const uint8_t byte : data) {
    crc ^= static_cast<uint32_t>(byte) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x80000000u)
        crc = (crc << 1) ^ 0x04C11DB7u;
      else
        crc <<= 1;
    }
  }
  return crc;
}

void appendTimestamp(std::vector<uint8_t>& out, uint8_t prefix, int64_t value)
{
  out.push_back(static_cast<uint8_t>((prefix << 4) | ((value >> 29) & 0x0E) | 0x01));
  out.push_back(static_cast<uint8_t>((value >> 22) & 0xFF));
  out.push_back(static_cast<uint8_t>(((value >> 14) & 0xFE) | 0x01));
  out.push_back(static_cast<uint8_t>((value >> 7) & 0xFF));
  out.push_back(static_cast<uint8_t>(((value << 1) & 0xFE) | 0x01));
}

} // namespace

TapoTsMuxer::TapoTsMuxer(TapoTsConfig config) : config_(std::move(config)) {}

void TapoTsMuxer::reset()
{
  patCounter_ = 0;
  pmtCounter_ = 0;
  audioCounter_ = 0;
}

std::vector<uint8_t> TapoTsMuxer::section(uint8_t tableId,
                                          const std::vector<uint8_t>& body)
{
  std::vector<uint8_t> out;
  out.push_back(tableId);
  const size_t length = body.size() + 4;
  out.push_back(static_cast<uint8_t>(0xB0 | ((length >> 8) & 0x0F)));
  out.push_back(static_cast<uint8_t>(length & 0xFF));
  out.insert(out.end(), body.begin(), body.end());

  const uint32_t crc = crc32Mpeg(out);
  out.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(crc & 0xFF));
  return out;
}

std::string TapoTsMuxer::packetize(uint16_t pid, const std::vector<uint8_t>& payload,
                                   bool sectionPayload, bool withPcr, int64_t pcr90k)
{
  std::string out;
  size_t offset = 0;
  bool first = true;
  uint8_t* counter = pid == kPatPid      ? &patCounter_
                     : pid == config_.pmtPid ? &pmtCounter_
                                             : &audioCounter_;

  while (offset < payload.size() || first) {
    std::vector<uint8_t> packet;
    packet.reserve(kPacketSize);
    packet.push_back(0x47);
    packet.push_back(static_cast<uint8_t>(((first ? 0x40 : 0x00)) | ((pid >> 8) & 0x1F)));
    packet.push_back(static_cast<uint8_t>(pid & 0xFF));

    const size_t prefix = (first && sectionPayload) ? 1 : 0;
    const size_t remaining = payload.size() - offset;

    bool hasAdaptation = false;
    std::vector<uint8_t> adaptation;
    if (first && withPcr) {
      hasAdaptation = true;
      adaptation.push_back(0x10);
      adaptation.push_back(static_cast<uint8_t>((pcr90k >> 25) & 0xFF));
      adaptation.push_back(static_cast<uint8_t>((pcr90k >> 17) & 0xFF));
      adaptation.push_back(static_cast<uint8_t>((pcr90k >> 9) & 0xFF));
      adaptation.push_back(static_cast<uint8_t>((pcr90k >> 1) & 0xFF));
      adaptation.push_back(static_cast<uint8_t>(((pcr90k & 0x01) << 7) | 0x7E));
      adaptation.push_back(0x00);
    }

    size_t capacity =
        kPacketSize - 4 - prefix - (hasAdaptation ? adaptation.size() + 1 : 0);
    if (remaining < capacity) {
      const size_t missing = capacity - remaining;
      if (hasAdaptation) {
        adaptation.insert(adaptation.end(), missing, 0xFF);
      } else if (missing == 1) {
        hasAdaptation = true;
      } else if (missing >= 2) {
        hasAdaptation = true;
        adaptation.push_back(0x00);
        adaptation.insert(adaptation.end(), missing - 2, 0xFF);
      }
      capacity = kPacketSize - 4 - prefix - (hasAdaptation ? adaptation.size() + 1 : 0);
    }

    const size_t take = std::min(capacity, remaining);
    const uint8_t control = static_cast<uint8_t>(hasAdaptation ? 0x30 : 0x10);
    packet.push_back(static_cast<uint8_t>(control | (*counter & 0x0F)));
    *counter = static_cast<uint8_t>((*counter + 1) & 0x0F);

    if (hasAdaptation) {
      packet.push_back(static_cast<uint8_t>(adaptation.size()));
      packet.insert(packet.end(), adaptation.begin(), adaptation.end());
    }
    if (prefix == 1)
      packet.push_back(0x00);

    packet.insert(packet.end(), payload.begin() + static_cast<long>(offset),
                  payload.begin() + static_cast<long>(offset + take));
    offset += take;

    packet.resize(kPacketSize, 0xFF);
    out.append(reinterpret_cast<const char*>(packet.data()), packet.size());
    first = false;
    if (offset >= payload.size())
      break;
  }
  return out;
}

std::string TapoTsMuxer::tables()
{
  std::vector<uint8_t> pat;
  pat.push_back(static_cast<uint8_t>((config_.programNumber >> 8) & 0xFF));
  pat.push_back(static_cast<uint8_t>(config_.programNumber & 0xFF));
  pat.push_back(0xC1);
  pat.push_back(0x00);
  pat.push_back(0x00);
  pat.push_back(static_cast<uint8_t>((config_.programNumber >> 8) & 0xFF));
  pat.push_back(static_cast<uint8_t>(config_.programNumber & 0xFF));
  pat.push_back(static_cast<uint8_t>(0xE0 | ((config_.pmtPid >> 8) & 0x1F)));
  pat.push_back(static_cast<uint8_t>(config_.pmtPid & 0xFF));

  std::vector<uint8_t> pmt;
  pmt.push_back(static_cast<uint8_t>((config_.programNumber >> 8) & 0xFF));
  pmt.push_back(static_cast<uint8_t>(config_.programNumber & 0xFF));
  pmt.push_back(0xC1);
  pmt.push_back(0x00);
  pmt.push_back(0x00);
  pmt.push_back(static_cast<uint8_t>(0xE0 | ((config_.audioPid >> 8) & 0x1F)));
  pmt.push_back(static_cast<uint8_t>(config_.audioPid & 0xFF));
  pmt.push_back(0xF0);
  pmt.push_back(0x00);
  pmt.push_back(config_.streamType);
  pmt.push_back(static_cast<uint8_t>(0xE0 | ((config_.audioPid >> 8) & 0x1F)));
  pmt.push_back(static_cast<uint8_t>(config_.audioPid & 0xFF));
  pmt.push_back(0xF0);
  pmt.push_back(0x00);

  return packetize(kPatPid, section(0x00, pat), true, false, 0) +
         packetize(config_.pmtPid, section(0x02, pmt), true, false, 0);
}

std::string TapoTsMuxer::frame(const TapoTsFrame& input)
{
  std::vector<uint8_t> pes;
  pes.push_back(0x00);
  pes.push_back(0x00);
  pes.push_back(0x01);
  pes.push_back(config_.streamId);

  const size_t packetLength = input.payload.size() + 8;
  pes.push_back(static_cast<uint8_t>((packetLength >> 8) & 0xFF));
  pes.push_back(static_cast<uint8_t>(packetLength & 0xFF));
  pes.push_back(0x80);
  pes.push_back(0x80);
  pes.push_back(0x05);
  appendTimestamp(pes, 0x02, input.pts90k);
  pes.insert(pes.end(), input.payload.begin(), input.payload.end());

  return packetize(config_.audioPid, pes, false, true, input.pts90k);
}
