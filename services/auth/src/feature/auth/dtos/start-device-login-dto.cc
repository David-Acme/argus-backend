#include "start-device-login-dto.hxx"

#include <algorithm>
#include <cctype>
#include <optional>

namespace
{
constexpr std::size_t kPollHashLength = 64;

bool isLowerHex(char value)
{
  return std::isdigit(static_cast<unsigned char>(value)) != 0 || (value >= 'a' && value <= 'f');
}
}

StartDeviceLoginDto StartDeviceLoginDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  StartDeviceLoginDto dto;
  if (const auto json = request->getJsonObject(); json && json->isObject())
    dto.pollHash = json->get("pollHash", "").asString();

  START_VALIDATION(StartDeviceLoginDto, dto)
  CUSTOM_LAMBDA(pollHash,
                [](const StartDeviceLoginDto& d) -> std::optional<std::string> {
                  if (d.pollHash.empty() ||
                      (d.pollHash.size() == kPollHashLength && std::ranges::all_of(d.pollHash, isLowerHex)))
                    return std::nullopt;
                  return "pollHash must be a lowercase SHA-256 hex digest";
                })
  END_VALIDATION()
  return dto;
}
