#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace tunnel::link
{
enum class FrameType : uint8_t
{
  Hello = 1,
  Open = 2,
  Data = 3,
  Close = 4,
  Ping = 5,
  Pong = 6,
  Push = 7,
  GoAway = 8,
  Credit = 9,
};

enum class CloseReason : uint8_t
{
  Normal = 0,
  Busy = 1,
  IdleTimeout = 2,
  Backpressure = 3,
  Error = 4,
};

enum class GoAwayReason : uint8_t
{
  NodeDrain = 0,
  NodeShutdown = 1,
  ProtocolError = 2,
};

enum class WireError : uint8_t
{
  BadMagic,
  BadVersion,
  UnknownFrameType,
  PayloadTooLarge,
  DataFrameTooLarge,
  StreamIdRequired,
  StreamIdForbidden,
  MalformedHello,
  UnsupportedVersion,
  MalformedOpen,
  CloseReasonRequired,
  UnknownCloseReason,
  MalformedPing,
  MalformedPong,
  MalformedGoAway,
  MalformedCredit,
  HelloRequired,
  DuplicateHello,
  UnexpectedOpen,
  OverCredit,
};

namespace capability
{
inline constexpr uint32_t kPushIntents = 0x00000001;
inline constexpr uint32_t kStreamCredit = 0x00000002;
inline constexpr uint32_t kKnown = kPushIntents | kStreamCredit;
}

inline constexpr uint16_t kMagic = 0xA755;
inline constexpr uint8_t kWireVersion = 2;
inline constexpr size_t kHeaderSize = 12;
inline constexpr size_t kMaxPayload = static_cast<size_t>(256) * 1024;
inline constexpr size_t kDataFrameMax = static_cast<size_t>(64) * 1024;
inline constexpr uint32_t kInitialStreamCredit =
    static_cast<uint32_t>(256) * 1024;
inline constexpr uint32_t kCreditGrantThreshold =
    static_cast<uint32_t>(64) * 1024;

std::string_view frameTypeToString(FrameType type) noexcept;
std::optional<FrameType> frameTypeFromString(std::string_view name) noexcept;
std::string_view closeReasonToString(CloseReason reason) noexcept;
std::optional<CloseReason> closeReasonFromString(std::string_view name) noexcept;
std::string_view goAwayReasonToString(GoAwayReason reason) noexcept;
std::optional<GoAwayReason> goAwayReasonFromString(
    std::string_view name) noexcept;
std::string_view wireErrorToString(WireError error) noexcept;
std::optional<WireError> wireErrorFromString(std::string_view name) noexcept;

struct Frame
{
  FrameType type{FrameType::Data};
  uint32_t streamId{0};
  std::string payload;
};

struct EncodeFrameInput
{
  FrameType type{FrameType::Data};
  uint32_t streamId{0};
  std::string_view payload;
};

struct CreditInput
{
  uint32_t streamId{0};
  uint32_t bytes{0};
};

struct HelloPayload
{
  uint16_t version{0};
  uint16_t reserved{0};
  uint32_t capabilities{0};
};

struct GoAwayPayload
{
  GoAwayReason reason{GoAwayReason::NodeDrain};
  uint16_t seconds{0};
};

std::string encodeFrame(const EncodeFrameInput& input);
std::string encodeHello(uint32_t capabilities);
std::string encodeOpen(uint32_t streamId);
std::string encodeData(uint32_t streamId, std::string_view bytes);
std::string encodeClose(uint32_t streamId, CloseReason reason);
std::string encodePush(std::string_view intent);
std::string encodePing(uint64_t nonce);
std::string encodePong(uint64_t nonce);
std::string encodeGoAway(GoAwayReason reason, uint16_t seconds);
std::string encodeCredit(const CreditInput& input);

HelloPayload decodeHello(const Frame& frame);
GoAwayPayload decodeGoAway(const Frame& frame);
uint64_t decodeNonce(const Frame& frame);
uint32_t decodeCredit(const Frame& frame);
CloseReason decodeCloseReason(const Frame& frame);

class FrameParser
{
public:
  void feed(std::string_view data);
  [[nodiscard]] bool hasFrame() const noexcept { return !frames_.empty(); }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] WireError error() const noexcept { return error_; }
  [[nodiscard]] Frame popFrame();

private:
  void parse();
  bool refuse(WireError error);

  std::string buffer_;
  std::deque<Frame> frames_;
  bool failed_{false};
  WireError error_{WireError::BadMagic};
};
}
