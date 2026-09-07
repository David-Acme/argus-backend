#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace tunnel
{
enum class FrameType : uint8_t
{
  Auth = 1,
  AuthOk = 2,
  AuthFail = 3,
  Open = 4,
  Data = 5,
  Close = 6,
  Ping = 7,
  Pong = 8,
  Push = 9,
  Challenge = 10,
};

inline constexpr uint16_t kMagic = 0xA755;
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kHeaderSize = 12;
inline constexpr size_t kMaxPayload = 256 * 1024;
inline constexpr size_t kDataFrameCap = 64 * 1024;
inline constexpr size_t kAuthPayloadSize = 32;
inline constexpr size_t kChallengeSize = 32;
inline constexpr uint8_t kMinFrameType = 1;
inline constexpr uint8_t kMaxFrameType = 10;

enum class CloseReason : uint8_t
{
  Normal = 0,
  Busy = 1,
  IdleTimeout = 2,
  Backpressure = 3,
  Error = 4,
};

struct Frame
{
  FrameType type{FrameType::Data};
  uint32_t streamId{0};
  std::string payload;
};

std::string encodeFrame(FrameType type, uint32_t streamId, const char* payload,
                        size_t size);
std::string encodeFrame(FrameType type, uint32_t streamId);

// Incremental frame parser; failed() marks an unrecoverable desync.
class FrameParser
{
public:
  void feed(const char* data, size_t size);
  void feed(const std::string& data) { feed(data.data(), data.size()); }
  bool hasFrame() const { return !frames_.empty(); }
  Frame popFrame();
  bool failed() const { return failed_; }

private:
  void parse();

  std::string buffer_;
  std::deque<Frame> frames_;
  bool failed_{false};
};

// HMAC-SHA256 over the shared secret; the home control plane's only
// authentication material. Both macs bind the per-link relay challenge, so
// captured material cannot be replayed on a later link.
std::string hmacSha256(const std::string& key, const std::string& message);
// Client proof: HMAC(secret, challenge || kAuthMessage).
std::string authMac(const std::string& secret, const std::string& challenge);
// Relay proof carried by AUTH_OK: HMAC(secret, challenge ||
// kRelayAuthMessage); the client verifies it before activating the link.
std::string relayAuthMac(const std::string& secret,
                         const std::string& challenge);
std::string randomChallenge();
bool constantTimeEquals(const std::string& left, const std::string& right);

inline constexpr char kAuthMessage[] = "argus-tunnel-auth-v1";
inline constexpr char kRelayAuthMessage[] = "argus-tunnel-relay-auth-v1";
} // namespace tunnel
