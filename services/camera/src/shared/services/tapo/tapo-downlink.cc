#include "tapo-downlink.hxx"

#include <cstring>
#include <shared/services/tapo/tapo-audio.hxx>

namespace
{

constexpr size_t kPacketSize = 188;
constexpr uint8_t kSyncByte = 0x47;
constexpr uint16_t kPatPid = 0;
constexpr uint16_t kNullPid = 0x1FFF;
constexpr uint8_t kStreamTypeALaw = 0x90;
constexpr uint8_t kStreamTypeULaw = 0x91;
constexpr int kALawRate = 8000;
constexpr int kULawRate = 16000;

uint16_t readUint16(const uint8_t* bytes)
{
  return static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
}

}

void TapoDownlink::feed(const std::vector<uint8_t>& bytes)
{
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void TapoDownlink::reset()
{
  buffer_.clear();
  pesPayload_.clear();
  pending_.clear();
}

TapoDownlinkCodec TapoDownlink::codec() const
{
  if (audioStreamType_ == kStreamTypeALaw)
    return TapoDownlinkCodec::ALaw;
  if (audioStreamType_ == kStreamTypeULaw)
    return TapoDownlinkCodec::ULaw;
  return TapoDownlinkCodec::None;
}

void TapoDownlink::parsePat(const uint8_t* payload)
{
  const uint8_t pointer = payload[0];
  const uint8_t* section = payload + 1 + pointer;
  if (section[0] != 0x00)
    return;
  const size_t sectionLength = readUint16(section + 1) & 0x0FFF;
  for (size_t at = 8; at + 4 <= 3 + sectionLength; at += 4) {
    const uint16_t program = readUint16(section + at);
    if (program != 0) {
      pmtPid_ = readUint16(section + at + 2) & 0x1FFF;
      return;
    }
  }
}

void TapoDownlink::parsePmt(const uint8_t* payload)
{
  const uint8_t pointer = payload[0];
  const uint8_t* section = payload + 1 + pointer;
  if (section[0] != 0x02)
    return;
  const size_t sectionLength = readUint16(section + 1) & 0x0FFF;
  const size_t end = 3 + sectionLength;
  const size_t infoLength = readUint16(section + 10) & 0x0FFF;
  size_t at = 12 + infoLength;
  while (at + 5 <= end) {
    const uint8_t streamType = section[at];
    const uint16_t pid = readUint16(section + at + 1) & 0x1FFF;
    const size_t esLength = readUint16(section + at + 3) & 0x0FFF;
    if (streamType == kStreamTypeALaw || streamType == kStreamTypeULaw) {
      audioStreamType_ = streamType;
      audioPid_ = pid;
      sampleRate_ = streamType == kStreamTypeALaw ? kALawRate : kULawRate;
      return;
    }
    at += 5 + esLength;
  }
}

void TapoDownlink::flushPes()
{
  if (!pesPayload_.empty()) {
    std::vector<int16_t> samples;
    if (audioStreamType_ == kStreamTypeALaw)
      samples = tapo_audio::decodeALaw(pesPayload_);
    else if (audioStreamType_ == kStreamTypeULaw)
      samples = tapo_audio::decodeULaw(pesPayload_);
    pending_.insert(pending_.end(), samples.begin(), samples.end());
    pesPayload_.clear();
  }
  pesRemaining_ = 0;
}

void TapoDownlink::appendPes(const uint8_t* payload, size_t size, bool start)
{
  if (size == 0)
    return;
  if (start) {
    flushPes();
    constexpr size_t kHeaderEnd = 9;
    if (size <= kHeaderEnd)
      return;
    const size_t offset = kHeaderEnd + payload[8];
    if (offset >= size)
      return;
    pesPayload_.assign(payload + offset, payload + size);
    pesRemaining_ =
        static_cast<int64_t>(readUint16(payload + 4)) - payload[8] - 3;
  }
  else {
    if (pesPayload_.empty())
      return;
    pesPayload_.insert(pesPayload_.end(), payload, payload + size);
  }
  if (pesRemaining_ <= 0 ||
      static_cast<int64_t>(pesPayload_.size()) < pesRemaining_)
    return;
  const auto take = static_cast<std::ptrdiff_t>(pesRemaining_);
  std::vector<uint8_t> es(pesPayload_.begin(), pesPayload_.begin() + take);
  pesPayload_ = std::move(es);
  flushPes();
}

void TapoDownlink::parsePacket(const uint8_t* packet)
{
  const auto pid =
      static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
  const bool start = (packet[1] & 0x40) != 0;
  if (pid == kNullPid)
    return;
  const auto adaptation = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
  size_t offset = 4;
  if (adaptation & 0x02)
    offset += 1 + packet[4];
  if (offset >= kPacketSize)
    return;
  const uint8_t* payload = packet + offset;
  const size_t size = kPacketSize - offset;
  if (pid == kPatPid) {
    parsePat(payload);
    return;
  }
  if (pmtPid_ != 0 && pid == pmtPid_) {
    parsePmt(payload);
    return;
  }
  if (audioPid_ != 0 && pid == audioPid_)
    appendPes(payload, size, start);
}

bool TapoDownlink::take(TapoDownlinkChunk& chunk)
{
  while (buffer_.size() >= kPacketSize) {
    if (buffer_[0] != kSyncByte) {
      buffer_.erase(buffer_.begin());
      continue;
    }
    parsePacket(buffer_.data());
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(kPacketSize));
  }
  if (pending_.empty())
    return false;
  chunk.samples = std::move(pending_);
  chunk.sampleRate = sampleRate_;
  chunk.codec = codec();
  pending_.clear();
  return true;
}
