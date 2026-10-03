#pragma once

#include <voice/voice-lang.hxx>

#include <cstdint>
#include <string_view>

enum class OfferReply : uint8_t
{
  Accept,
  Decline,
  Other
};

[[nodiscard]] OfferReply offerReplyOf(std::string_view text, VoiceLang lang);
