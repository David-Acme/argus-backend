#include "ws-frame.hxx"

#include <cstring>

namespace ws_frame
{

bool parse(const uint8_t* data, size_t len, Header& out)
{
  if (len < kHeaderSize || data[0] != kMagic || data[1] != kVersion)
    return false;

  Header h;
  h.type = data[2];
  h.keyframe = (data[3] & kFlagKeyframe) != 0;
  h.subId = static_cast<uint16_t>((data[4] << 8) | data[5]);
  h.seq = (static_cast<uint32_t>(data[8]) << 24) |
          (static_cast<uint32_t>(data[9]) << 16) |
          (static_cast<uint32_t>(data[10]) << 8) |
          static_cast<uint32_t>(data[11]);
  out = h;
  return true;
}

void encode(uint8_t* out, const Header& header)
{
  std::memset(out, 0, kHeaderSize);
  out[0] = kMagic;
  out[1] = kVersion;
  out[2] = header.type;
  if (header.keyframe)
    out[3] |= kFlagKeyframe;
  out[4] = static_cast<uint8_t>((header.subId >> 8) & 0xFF);
  out[5] = static_cast<uint8_t>(header.subId & 0xFF);
  out[8] = static_cast<uint8_t>((header.seq >> 24) & 0xFF);
  out[9] = static_cast<uint8_t>((header.seq >> 16) & 0xFF);
  out[10] = static_cast<uint8_t>((header.seq >> 8) & 0xFF);
  out[11] = static_cast<uint8_t>(header.seq & 0xFF);
}

std::string frame(const Header& header, const uint8_t* payload, size_t len)
{
  std::string out;
  out.resize(kHeaderSize + len);
  encode(reinterpret_cast<uint8_t*>(out.data()), header);
  if (len > 0)
    std::memcpy(out.data() + kHeaderSize, payload, len);
  return out;
}

} // namespace ws_frame
