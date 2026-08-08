#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ws_frame
{
constexpr uint8_t kMagic = 0xA7;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kTypeInit = 1;
constexpr uint8_t kTypeMedia = 2;
constexpr uint8_t kTypeAudio = 3;
constexpr uint8_t kFlagKeyframe = 0x01;
constexpr size_t kHeaderSize = 12;

struct Header
{
  uint8_t type{kTypeMedia};
  bool keyframe{false};
  uint16_t subId{0};
  uint32_t seq{0};
};

bool parse(const uint8_t* data, size_t len, Header& out);
void encode(uint8_t* out, const Header& header);
std::string frame(const Header& header, const uint8_t* payload, size_t len);

} // namespace ws_frame
