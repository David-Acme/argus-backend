#pragma once

#include <feature/safety/vocabulary/phrase-change.hxx>

#include <json/value.h>
#include <optional>
#include <string>

struct UpdateSafetyDto
{
  bool duressEnabled{false};
  std::optional<std::string> currentPin;
  PhraseChange phraseChange{PhraseChange::Keep};
  std::string duressPhrase;

  static UpdateSafetyDto fromJson(const Json::Value& json);
};
