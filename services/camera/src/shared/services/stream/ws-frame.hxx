#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

struct ParseInput
{
  const uint8_t* data{nullptr};
  size_t len{0};
};

std::optional<Header> parse(const ParseInput& input);
void encode(uint8_t* out, const Header& header);

struct FrameInput
{
  const Header& header;
  const uint8_t* payload{nullptr};
  size_t len{0};
};

std::string frame(const FrameInput& input);

} // namespace ws_frame
