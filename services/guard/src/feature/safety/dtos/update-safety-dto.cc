#include "update-safety-dto.hxx"

#include <feature/safety/services/pin-hash.hxx>

#include <text/secret-phrase.hxx>
#include <validation/validation_dsl.hxx>

#include <algorithm>
#include <cctype>

namespace
{
constexpr size_t kMaxPhraseBytes = 200;

bool blank(const std::string& text)
{
  return std::ranges::all_of(text, [](char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; });
}

bool phraseShaped(const std::string& phrase)
{
  if (phrase.size() > kMaxPhraseBytes)
    return false;
  const bool plain = std::ranges::none_of(phrase, [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isdigit(byte) != 0 || std::iscntrl(byte) != 0;
  });
  const size_t count = secret_phrase::words(phrase).size();
  return plain && count >= secret_phrase::kMinWords && count <= secret_phrase::kMaxWords;
}
}

UpdateSafetyDto UpdateSafetyDto::fromJson(const Json::Value& json)
{
  UpdateSafetyDto dto;
  const bool present = json.isMember("duressEnabled") && json["duressEnabled"].isBool();
  if (present)
    dto.duressEnabled = json["duressEnabled"].asBool();
  const bool hasCurrent = json.isMember("currentPin") && !json["currentPin"].isNull();
  if (hasCurrent && json["currentPin"].isString())
    dto.currentPin = json["currentPin"].asString();
  const bool hasPhrase = json.isMember("duressPhrase");
  const bool phraseText = !hasPhrase || json["duressPhrase"].isNull() || json["duressPhrase"].isString();
  if (hasPhrase && phraseText) {
    dto.duressPhrase = json["duressPhrase"].isString() ? json["duressPhrase"].asString() : std::string{};
    dto.phraseChange = blank(dto.duressPhrase) ? PhraseChange::Clear : PhraseChange::Set;
  }

  START_VALIDATION(UpdateSafetyDto, dto)
  CUSTOM_LAMBDA(duressEnabled, [present](const UpdateSafetyDto&) -> std::optional<std::string> {
    if (!present)
      return "must be a boolean";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(currentPin, [hasCurrent](const UpdateSafetyDto& value) -> std::optional<std::string> {
    if (hasCurrent && (!value.currentPin || !pin_hash::wellFormedPin(*value.currentPin)))
      return "must be 4 to 8 digits";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(duressPhrase, [phraseText](const UpdateSafetyDto& value) -> std::optional<std::string> {
    if (!phraseText)
      return "must be a string or null";
    if (value.phraseChange != PhraseChange::Set)
      return std::nullopt;
    if (!value.duressEnabled)
      return "needs duressEnabled";
    if (!phraseShaped(value.duressPhrase))
      return "must be 3 to 12 words without digits";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
