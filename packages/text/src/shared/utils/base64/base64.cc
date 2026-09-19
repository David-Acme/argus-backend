#include <shared/utils/base64/base64.hxx>

#include <cstdint>

namespace
{
constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

std::string base64::encode(std::string_view bytes)
{
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < bytes.size()) {
    const uint32_t block =
        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 1])) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 2]));
    encoded.push_back(kAlphabet[(block >> 18) & 0x3F]);
    encoded.push_back(kAlphabet[(block >> 12) & 0x3F]);
    encoded.push_back(kAlphabet[(block >> 6) & 0x3F]);
    encoded.push_back(kAlphabet[block & 0x3F]);
    i += 3;
  }

  if (i >= bytes.size())
    return encoded;

  uint32_t block = static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) << 16;
  encoded.push_back(kAlphabet[(block >> 18) & 0x3F]);
  if (i + 1 < bytes.size()) {
    block |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 1])) << 8;
    encoded.push_back(kAlphabet[(block >> 12) & 0x3F]);
    encoded.push_back(kAlphabet[(block >> 6) & 0x3F]);
    encoded.push_back('=');
  }
  else {
    encoded.push_back(kAlphabet[(block >> 12) & 0x3F]);
    encoded.push_back('=');
    encoded.push_back('=');
  }
  return encoded;
}

std::optional<std::string> base64::decode(std::string_view text)
{
  if (text.size() % 4 != 0)
    return std::nullopt;

  const auto valueOf = [](char character) -> int {
    if (character >= 'A' && character <= 'Z')
      return character - 'A';
    if (character >= 'a' && character <= 'z')
      return character - 'a' + 26;
    if (character >= '0' && character <= '9')
      return character - '0' + 52;
    if (character == '+')
      return 62;
    if (character == '/')
      return 63;
    return -1;
  };

  std::string decoded;
  decoded.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    int block = 0;
    int padding = 0;
    for (size_t j = 0; j < 4; ++j) {
      const char character = text[i + j];
      if (character == '=') {
        ++padding;
        block <<= 6;
        continue;
      }
      const int value = valueOf(character);
      if (value < 0)
        return std::nullopt;
      block = (block << 6) | value;
    }
    decoded.push_back(static_cast<char>((block >> 16) & 0xFF));
    if (padding < 2)
      decoded.push_back(static_cast<char>((block >> 8) & 0xFF));
    if (padding < 1)
      decoded.push_back(static_cast<char>(block & 0xFF));
  }
  return decoded;
}
