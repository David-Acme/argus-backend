#include <link/protocol/frames.hxx>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace tunnel::link
{
namespace
{
template <typename T>
struct Named
{
  std::string_view name;
  T value;
};

constexpr std::array<Named<FrameType>, 9> kFrameTypes{{
    {.name = "hello", .value = FrameType::Hello},
    {.name = "open", .value = FrameType::Open},
    {.name = "data", .value = FrameType::Data},
    {.name = "close", .value = FrameType::Close},
    {.name = "ping", .value = FrameType::Ping},
    {.name = "pong", .value = FrameType::Pong},
    {.name = "push", .value = FrameType::Push},
    {.name = "goaway", .value = FrameType::GoAway},
    {.name = "credit", .value = FrameType::Credit},
}};

constexpr std::array<Named<CloseReason>, 5> kCloseReasons{{
    {.name = "normal", .value = CloseReason::Normal},
    {.name = "busy", .value = CloseReason::Busy},
    {.name = "idle_timeout", .value = CloseReason::IdleTimeout},
    {.name = "backpressure", .value = CloseReason::Backpressure},
    {.name = "error", .value = CloseReason::Error},
}};

constexpr std::array<Named<GoAwayReason>, 3> kGoAwayReasons{{
    {.name = "node_drain", .value = GoAwayReason::NodeDrain},
    {.name = "node_shutdown", .value = GoAwayReason::NodeShutdown},
    {.name = "protocol_error", .value = GoAwayReason::ProtocolError},
}};

constexpr std::array<Named<WireError>, 20> kWireErrors{{
    {.name = "bad_magic", .value = WireError::BadMagic},
    {.name = "bad_version", .value = WireError::BadVersion},
    {.name = "unknown_frame_type", .value = WireError::UnknownFrameType},
    {.name = "payload_too_large", .value = WireError::PayloadTooLarge},
    {.name = "data_frame_too_large", .value = WireError::DataFrameTooLarge},
    {.name = "stream_id_required", .value = WireError::StreamIdRequired},
    {.name = "stream_id_forbidden", .value = WireError::StreamIdForbidden},
    {.name = "malformed_hello", .value = WireError::MalformedHello},
    {.name = "unsupported_version", .value = WireError::UnsupportedVersion},
    {.name = "malformed_open", .value = WireError::MalformedOpen},
    {.name = "close_reason_required", .value = WireError::CloseReasonRequired},
    {.name = "unknown_close_reason", .value = WireError::UnknownCloseReason},
    {.name = "malformed_ping", .value = WireError::MalformedPing},
    {.name = "malformed_pong", .value = WireError::MalformedPong},
    {.name = "malformed_goaway", .value = WireError::MalformedGoAway},
    {.name = "malformed_credit", .value = WireError::MalformedCredit},
    {.name = "hello_required", .value = WireError::HelloRequired},
    {.name = "duplicate_hello", .value = WireError::DuplicateHello},
    {.name = "unexpected_open", .value = WireError::UnexpectedOpen},
    {.name = "over_credit", .value = WireError::OverCredit},
}};

template <typename T, size_t N>
std::string_view nameOf(const std::array<Named<T>, N>& table, T value) noexcept
{
  for (const auto& entry : table) {
    if (entry.value == value)
      return entry.name;
  }
  return "unknown";
}

template <typename T, size_t N>
std::optional<T> fromName(const std::array<Named<T>, N>& table,
                          std::string_view name) noexcept
{
  for (const auto& entry : table) {
    if (entry.name == name)
      return entry.value;
  }
  return std::nullopt;
}

void appendU16(std::string& out, uint16_t value)
{
  const uint32_t raw = value;
  out.push_back(static_cast<char>(raw & 0xFFU));
  out.push_back(static_cast<char>((raw >> 8U) & 0xFFU));
}

void appendU32(std::string& out, uint32_t value)
{
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

uint16_t readU16(const char* data)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  const uint32_t low = bytes[0];
  const uint32_t high = bytes[1];
  return static_cast<uint16_t>(low | (high << 8U));
}

uint32_t readU32(const char* data)
{
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  const uint32_t first = bytes[0];
  const uint32_t second = bytes[1];
  const uint32_t third = bytes[2];
  const uint32_t fourth = bytes[3];
  return first | (second << 8U) | (third << 16U) | (fourth << 24U);
}

bool isControlFrame(FrameType type)
{
  switch (type) {
  case FrameType::Hello:
  case FrameType::Ping:
  case FrameType::Pong:
  case FrameType::Push:
  case FrameType::GoAway:
    return true;
  case FrameType::Open:
  case FrameType::Data:
  case FrameType::Close:
  case FrameType::Credit:
    return false;
  }
  return false;
}

std::optional<WireError> lengthRefusal(FrameType type, uint32_t length)
{
  switch (type) {
  case FrameType::Hello:
    return length == 8 ? std::nullopt
                       : std::optional{WireError::MalformedHello};
  case FrameType::Open:
    return length == 0 ? std::nullopt
                       : std::optional{WireError::MalformedOpen};
  case FrameType::Close:
    return length == 1 ? std::nullopt
                       : std::optional{WireError::CloseReasonRequired};
  case FrameType::Ping:
    return length == 8 ? std::nullopt
                       : std::optional{WireError::MalformedPing};
  case FrameType::Pong:
    return length == 8 ? std::nullopt
                       : std::optional{WireError::MalformedPong};
  case FrameType::GoAway:
    return length == 4 ? std::nullopt
                       : std::optional{WireError::MalformedGoAway};
  case FrameType::Credit:
    return length == 4 ? std::nullopt
                       : std::optional{WireError::MalformedCredit};
  case FrameType::Data:
  case FrameType::Push:
    return std::nullopt;
  }
  return std::nullopt;
}
}

std::string_view frameTypeToString(FrameType type) noexcept
{
  return nameOf(kFrameTypes, type);
}

std::optional<FrameType> frameTypeFromString(std::string_view name) noexcept
{
  return fromName(kFrameTypes, name);
}

std::string_view closeReasonToString(CloseReason reason) noexcept
{
  return nameOf(kCloseReasons, reason);
}

std::optional<CloseReason> closeReasonFromString(std::string_view name) noexcept
{
  return fromName(kCloseReasons, name);
}

std::string_view goAwayReasonToString(GoAwayReason reason) noexcept
{
  return nameOf(kGoAwayReasons, reason);
}

std::optional<GoAwayReason> goAwayReasonFromString(
    std::string_view name) noexcept
{
  return fromName(kGoAwayReasons, name);
}

std::string_view wireErrorToString(WireError error) noexcept
{
  return nameOf(kWireErrors, error);
}

std::optional<WireError> wireErrorFromString(std::string_view name) noexcept
{
  return fromName(kWireErrors, name);
}

std::string encodeFrame(const EncodeFrameInput& input)
{
  std::string out;
  out.reserve(kHeaderSize + input.payload.size());
  appendU16(out, kMagic);
  out.push_back(static_cast<char>(kWireVersion));
  out.push_back(static_cast<char>(input.type));
  appendU32(out, input.streamId);
  appendU32(out, static_cast<uint32_t>(input.payload.size()));
  out.append(input.payload);
  return out;
}

std::string encodeHello(uint32_t capabilities)
{
  std::string payload;
  payload.reserve(8);
  appendU16(payload, kWireVersion);
  appendU16(payload, 0);
  appendU32(payload, capabilities & capability::kKnown);
  return encodeFrame(
      {.type = FrameType::Hello, .streamId = 0, .payload = payload});
}

std::string encodeOpen(uint32_t streamId)
{
  return encodeFrame({.type = FrameType::Open,
                      .streamId = streamId,
                      .payload = std::string_view{}});
}

std::string encodeData(uint32_t streamId, std::string_view bytes)
{
  return encodeFrame(
      {.type = FrameType::Data, .streamId = streamId, .payload = bytes});
}

std::string encodeClose(uint32_t streamId, CloseReason reason)
{
  const auto raw = static_cast<uint8_t>(reason);
  const char byte = static_cast<char>(raw);
  return encodeFrame(
      {.type = FrameType::Close,
       .streamId = streamId,
       .payload = std::string_view{&byte, 1}});
}

std::string encodePush(std::string_view intent)
{
  return encodeFrame(
      {.type = FrameType::Push, .streamId = 0, .payload = intent});
}

std::string encodePing(uint64_t nonce)
{
  std::string payload;
  payload.reserve(8);
  for (uint32_t index = 0; index < 8; ++index)
    payload.push_back(static_cast<char>((nonce >> (index * 8U)) & 0xFFU));
  return encodeFrame(
      {.type = FrameType::Ping, .streamId = 0, .payload = payload});
}

std::string encodePong(uint64_t nonce)
{
  std::string frame = encodePing(nonce);
  frame[3] = static_cast<char>(FrameType::Pong);
  return frame;
}

std::string encodeGoAway(GoAwayReason reason, uint16_t seconds)
{
  std::string payload;
  payload.reserve(4);
  appendU16(payload, static_cast<uint16_t>(reason));
  appendU16(payload, seconds);
  return encodeFrame(
      {.type = FrameType::GoAway, .streamId = 0, .payload = payload});
}

std::string encodeCredit(const CreditInput& input)
{
  std::string payload;
  payload.reserve(4);
  appendU32(payload, input.bytes);
  return encodeFrame({.type = FrameType::Credit,
                      .streamId = input.streamId,
                      .payload = payload});
}

HelloPayload decodeHello(const Frame& frame)
{
  const char* data = frame.payload.data();
  return {.version = readU16(data),
          .reserved = readU16(data + 2),
          .capabilities = readU32(data + 4)};
}

GoAwayPayload decodeGoAway(const Frame& frame)
{
  const char* data = frame.payload.data();
  return {.reason = static_cast<GoAwayReason>(readU16(data)),
          .seconds = readU16(data + 2)};
}

uint64_t decodeNonce(const Frame& frame)
{
  const auto* data = reinterpret_cast<const uint8_t*>(frame.payload.data());
  uint64_t nonce = 0;
  for (int index = 7; index >= 0; --index)
    nonce = (nonce << 8U) | static_cast<uint64_t>(data[index]);
  return nonce;
}

uint32_t decodeCredit(const Frame& frame)
{
  return readU32(frame.payload.data());
}

CloseReason decodeCloseReason(const Frame& frame)
{
  const auto raw = static_cast<uint8_t>(frame.payload.front());
  return static_cast<CloseReason>(raw);
}

void FrameParser::feed(std::string_view data)
{
  if (failed_ || data.empty())
    return;
  buffer_.append(data);
  parse();
}

bool FrameParser::refuse(WireError error)
{
  failed_ = true;
  error_ = error;
  buffer_.clear();
  frames_.clear();
  return false;
}

void FrameParser::parse()
{
  size_t offset = 0;
  while (buffer_.size() - offset >= kHeaderSize) {
    const char* header = buffer_.data() + offset;
    if (readU16(header) != kMagic) {
      refuse(WireError::BadMagic);
      return;
    }
    if (static_cast<uint8_t>(header[2]) != kWireVersion) {
      refuse(WireError::BadVersion);
      return;
    }
    const auto rawType = static_cast<uint8_t>(header[3]);
    if (rawType < static_cast<uint8_t>(FrameType::Hello) ||
        rawType > static_cast<uint8_t>(FrameType::Credit)) {
      refuse(WireError::UnknownFrameType);
      return;
    }
    const auto type = static_cast<FrameType>(rawType);
    const uint32_t streamId = readU32(header + 4);
    const uint32_t payloadSize = readU32(header + 8);
    if (payloadSize > kMaxPayload) {
      refuse(WireError::PayloadTooLarge);
      return;
    }
    if (isControlFrame(type)) {
      if (streamId != 0) {
        refuse(WireError::StreamIdForbidden);
        return;
      }
    } else if (streamId == 0) {
      refuse(WireError::StreamIdRequired);
      return;
    }
    if (type == FrameType::Data && payloadSize > kDataFrameMax) {
      refuse(WireError::DataFrameTooLarge);
      return;
    }
    if (const auto refusal = lengthRefusal(type, payloadSize)) {
      refuse(*refusal);
      return;
    }
    const size_t frameSize = kHeaderSize + payloadSize;
    if (buffer_.size() - offset < frameSize)
      break;
    Frame frame;
    frame.type = type;
    frame.streamId = streamId;
    frame.payload.assign(buffer_.data() + offset + kHeaderSize, payloadSize);
    offset += frameSize;
    switch (type) {
    case FrameType::Hello: {
      const HelloPayload hello = decodeHello(frame);
      if (hello.version != kWireVersion) {
        refuse(WireError::UnsupportedVersion);
        return;
      }
      if (hello.reserved != 0) {
        refuse(WireError::MalformedHello);
        return;
      }
      break;
    }
    case FrameType::Close: {
      const auto reason = decodeCloseReason(frame);
      const auto raw = static_cast<uint8_t>(reason);
      if (raw > static_cast<uint8_t>(CloseReason::Error)) {
        refuse(WireError::UnknownCloseReason);
        return;
      }
      break;
    }
    case FrameType::Credit:
      if (decodeCredit(frame) == 0) {
        refuse(WireError::MalformedCredit);
        return;
      }
      break;
    case FrameType::GoAway:
      if (static_cast<uint16_t>(decodeGoAway(frame).reason) >
          static_cast<uint16_t>(GoAwayReason::ProtocolError)) {
        refuse(WireError::MalformedGoAway);
        return;
      }
      break;
    case FrameType::Open:
    case FrameType::Data:
    case FrameType::Ping:
    case FrameType::Pong:
    case FrameType::Push:
      break;
    }
    frames_.push_back(std::move(frame));
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
}
