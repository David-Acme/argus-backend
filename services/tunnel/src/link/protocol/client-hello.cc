#include <link/protocol/client-hello.hxx>

#include <algorithm>
#include <utility>

namespace tunnel::link
{
namespace
{
constexpr uint8_t kHandshakeRecord = 0x16U;
constexpr uint8_t kAlertRecord = 0x15U;
constexpr uint8_t kAlertFatal = 0x02U;
constexpr uint8_t kClientHelloMessage = 0x01U;
constexpr uint16_t kServerNameExtension = 0x0000U;
constexpr uint8_t kHostNameType = 0x00U;
constexpr size_t kRecordHeader = 5;
constexpr size_t kHandshakeHeader = 4;
constexpr size_t kVersionAndRandom = 34;
constexpr size_t kExtensionHeader = 4;
constexpr size_t kServerNameEntryHeader = 3;
constexpr size_t kHomeIdLength = 32;
constexpr size_t kMaxLabelLength = 63;
constexpr size_t kMinServiceLength = 3;
constexpr size_t kMaxServiceLength = 12;
constexpr std::string_view kServiceSeparator = "--";

class Reader
{
public:
  explicit Reader(std::string_view bytes) : bytes_{bytes} {}

  [[nodiscard]] size_t left() const { return bytes_.size() - cursor_; }
  [[nodiscard]] bool has(size_t count) const { return left() >= count; }

  uint32_t byte()
  {
    const auto value = static_cast<uint8_t>(bytes_[cursor_]);
    ++cursor_;
    return value;
  }

  uint32_t twoBytes()
  {
    const uint32_t high = byte();
    return (high << 8U) | byte();
  }

  uint32_t threeBytes()
  {
    const uint32_t high = twoBytes();
    return (high << 8U) | byte();
  }

  std::string_view take(size_t count)
  {
    const std::string_view part = bytes_.substr(cursor_, count);
    cursor_ += part.size();
    return part;
  }

  bool skip(size_t count)
  {
    if (!has(count))
      return false;
    cursor_ += count;
    return true;
  }

private:
  std::string_view bytes_;
  size_t cursor_{0};
};

ClientHelloResult malformedHello()
{
  return {.status = ClientHelloStatus::Malformed, .serverName = {}};
}

std::optional<std::string> hostNameIn(std::string_view extension)
{
  Reader reader{extension};
  if (!reader.has(2))
    return std::nullopt;
  const uint32_t listLength = reader.twoBytes();
  if (!reader.has(listLength))
    return std::nullopt;
  Reader list{reader.take(listLength)};
  std::string hostName;
  while (list.has(kServerNameEntryHeader)) {
    const uint32_t type = list.byte();
    const uint32_t length = list.twoBytes();
    if (!list.has(length))
      return std::nullopt;
    const std::string_view name = list.take(length);
    if (type == kHostNameType && hostName.empty())
      hostName.assign(name);
  }
  return hostName;
}

char lowered(char letter)
{
  return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a')
                                        : letter;
}

bool sameIgnoringCase(std::string_view lhs, std::string_view rhs)
{
  return lhs.size() == rhs.size() &&
         std::ranges::equal(lhs, rhs, [](char left, char right) {
           return lowered(left) == lowered(right);
         });
}

bool isHomeId(std::string_view text)
{
  return text.size() == kHomeIdLength &&
         std::ranges::all_of(text, [](char letter) {
           return (letter >= '0' && letter <= '9') ||
                  (letter >= 'a' && letter <= 'f');
         });
}

bool isService(std::string_view text)
{
  return text.size() >= kMinServiceLength && text.size() <= kMaxServiceLength &&
         text.front() >= 'a' && text.front() <= 'z' &&
         std::ranges::all_of(text, [](char letter) {
           return (letter >= '0' && letter <= '9') ||
                  (letter >= 'a' && letter <= 'z');
         });
}
}

ClientHelloResult parseClientHello(std::string_view bytes)
{
  if (bytes.size() < kRecordHeader)
    return {};
  Reader record{bytes};
  if (record.byte() != kHandshakeRecord)
    return malformedHello();
  record.skip(2);
  const uint32_t recordLength = record.twoBytes();
  if (recordLength < kHandshakeHeader)
    return malformedHello();
  if (!record.has(recordLength))
    return {};
  Reader handshake{record.take(recordLength)};
  if (handshake.byte() != kClientHelloMessage)
    return malformedHello();
  const uint32_t messageLength = handshake.threeBytes();
  if (!handshake.has(messageLength))
    return malformedHello();
  Reader hello{handshake.take(messageLength)};
  if (!hello.skip(kVersionAndRandom) || !hello.has(1) ||
      !hello.skip(hello.byte()) || !hello.has(2) ||
      !hello.skip(hello.twoBytes()) || !hello.has(1) ||
      !hello.skip(hello.byte()))
    return malformedHello();
  if (hello.left() == 0)
    return {.status = ClientHelloStatus::Parsed, .serverName = {}};
  if (!hello.has(2))
    return malformedHello();
  const uint32_t extensionsLength = hello.twoBytes();
  if (!hello.has(extensionsLength))
    return malformedHello();
  Reader extensions{hello.take(extensionsLength)};
  std::string serverName;
  while (extensions.has(kExtensionHeader)) {
    const uint32_t type = extensions.twoBytes();
    const uint32_t length = extensions.twoBytes();
    if (!extensions.has(length))
      return malformedHello();
    const std::string_view data = extensions.take(length);
    if (type != kServerNameExtension)
      continue;
    std::optional<std::string> name = hostNameIn(data);
    if (!name)
      return malformedHello();
    if (serverName.empty())
      serverName = std::move(*name);
  }
  return {.status = ClientHelloStatus::Parsed, .serverName = std::move(serverName)};
}

std::optional<DeviceName> parseDeviceName(const DeviceNameInput& input)
{
  if (input.serverName.size() <= input.domainSuffix.size())
    return std::nullopt;
  const size_t labelLength = input.serverName.size() - input.domainSuffix.size();
  if (labelLength > kMaxLabelLength)
    return std::nullopt;
  if (!sameIgnoringCase(input.serverName.substr(labelLength), input.domainSuffix))
    return std::nullopt;
  std::string label{input.serverName.substr(0, labelLength)};
  std::ranges::transform(label, label.begin(), lowered);
  const size_t separator = label.rfind(kServiceSeparator);
  const bool named = separator != std::string::npos;
  const std::string_view whole = label;
  const std::string_view homeId =
      named ? whole.substr(separator + kServiceSeparator.size()) : whole;
  const std::string_view service =
      named ? whole.substr(0, separator) : std::string_view{};
  if (!isHomeId(homeId) || (named && !isService(service)))
    return std::nullopt;
  return DeviceName{.service = std::string{service}, .homeId = std::string{homeId}};
}

std::string tlsAlertRecord(uint8_t description)
{
  std::string record;
  for (const uint8_t byte : {kAlertRecord, uint8_t{0x03U}, uint8_t{0x03U},
                             uint8_t{0x00U}, uint8_t{0x02U}, kAlertFatal})
    record.push_back(static_cast<char>(byte));
  record.push_back(static_cast<char>(description));
  return record;
}
}
