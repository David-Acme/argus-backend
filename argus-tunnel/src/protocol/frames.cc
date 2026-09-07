#include "frames.hxx"

#include <trantor/utils/Utilities.h>

#include <cstring>

namespace tunnel
{
namespace
{
void appendU16(std::string& out, uint16_t value)
{
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void appendU32(std::string& out, uint32_t value)
{
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

uint16_t readU16(const char* data)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1] << 8);
}

uint32_t readU32(const char* data)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1] << 8) |
         static_cast<uint32_t>(bytes[2] << 16) |
         static_cast<uint32_t>(bytes[3] << 24);
}
} // namespace

std::string encodeFrame(FrameType type, uint32_t streamId, const char* payload,
                        size_t size)
{
  std::string out;
  out.reserve(kHeaderSize + size);
  appendU16(out, kMagic);
  out.push_back(static_cast<char>(kVersion));
  out.push_back(static_cast<char>(type));
  appendU32(out, streamId);
  appendU32(out, static_cast<uint32_t>(size));
  out.append(payload, size);
  return out;
}

std::string encodeFrame(FrameType type, uint32_t streamId)
{
  return encodeFrame(type, streamId, nullptr, 0);
}

void FrameParser::feed(const char* data, size_t size)
{
  if (failed_)
    return;
  buffer_.append(data, size);
  parse();
}

void FrameParser::parse()
{
  size_t offset = 0;
  while (buffer_.size() - offset >= kHeaderSize) {
    const char* header = buffer_.data() + offset;
    if (readU16(header) != kMagic ||
        static_cast<uint8_t>(header[2]) != kVersion) {
      failed_ = true;
      break;
    }
    const auto type = static_cast<uint8_t>(header[3]);
    const uint32_t streamId = readU32(header + 4);
    const uint32_t payloadSize = readU32(header + 8);
    if (type < kMinFrameType || type > kMaxFrameType ||
        payloadSize > kMaxPayload) {
      failed_ = true;
      break;
    }
    const size_t frameSize = kHeaderSize + payloadSize;
    if (buffer_.size() - offset < frameSize)
      break;
    Frame frame;
    frame.type = static_cast<FrameType>(type);
    frame.streamId = streamId;
    frame.payload.assign(buffer_.data() + offset + kHeaderSize, payloadSize);
    frames_.push_back(std::move(frame));
    offset += frameSize;
  }
  if (offset > 0)
    buffer_.erase(0, offset);
}

Frame FrameParser::popFrame()
{
  Frame frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

std::string hmacSha256(const std::string& key, const std::string& message)
{
  using trantor::utils::Hash256;
  using trantor::utils::sha256;

  std::string block = key;
  if (block.size() > 64)
    block = std::string(reinterpret_cast<const char*>(sha256(block).bytes), 32);
  block.resize(64, '\0');

  std::string inner(64, '\0');
  std::string outer(64, '\0');
  for (size_t i = 0; i < 64; ++i) {
    inner[i] = static_cast<char>(block[i] ^ 0x36);
    outer[i] = static_cast<char>(block[i] ^ 0x5C);
  }
  inner.append(message);
  Hash256 innerHash = sha256(inner);
  outer.append(reinterpret_cast<const char*>(innerHash.bytes), 32);
  Hash256 mac = sha256(outer);
  return std::string(reinterpret_cast<const char*>(mac.bytes), 32);
}

std::string authMac(const std::string& secret)
{
  return hmacSha256(secret, kAuthMessage);
}

bool constantTimeEquals(const std::string& left, const std::string& right)
{
  if (left.size() != right.size())
    return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < left.size(); ++i)
    diff |= static_cast<unsigned char>(left[i]) ^
            static_cast<unsigned char>(right[i]);
  return diff == 0;
}
} // namespace tunnel
