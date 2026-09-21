#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class TtsLang
{
  EN,
  KO,
  JA,
  AR,
  BG,
  CS,
  DA,
  DE,
  EL,
  ES,
  ET,
  FI,
  FR,
  HI,
  HR,
  HU,
  ID,
  IT,
  LT,
  LV,
  NL,
  PL,
  PT,
  RO,
  RU,
  SK,
  SL,
  SV,
  TR,
  UK,
  VI,
  NA
};

// Enumerator count (EN..NA): the wire language mapping iterates the full set.
constexpr int kTtsLangCount = static_cast<int>(TtsLang::NA) + 1;
static_assert(kTtsLangCount == 32, "langCode/supportedLangCodes cover TtsLang");

constexpr const char* langCode(TtsLang lang)
{
  switch (lang) {
    case TtsLang::EN:
      return "en";
    case TtsLang::KO:
      return "ko";
    case TtsLang::JA:
      return "ja";
    case TtsLang::AR:
      return "ar";
    case TtsLang::BG:
      return "bg";
    case TtsLang::CS:
      return "cs";
    case TtsLang::DA:
      return "da";
    case TtsLang::DE:
      return "de";
    case TtsLang::EL:
      return "el";
    case TtsLang::ES:
      return "es";
    case TtsLang::ET:
      return "et";
    case TtsLang::FI:
      return "fi";
    case TtsLang::FR:
      return "fr";
    case TtsLang::HI:
      return "hi";
    case TtsLang::HR:
      return "hr";
    case TtsLang::HU:
      return "hu";
    case TtsLang::ID:
      return "id";
    case TtsLang::IT:
      return "it";
    case TtsLang::LT:
      return "lt";
    case TtsLang::LV:
      return "lv";
    case TtsLang::NL:
      return "nl";
    case TtsLang::PL:
      return "pl";
    case TtsLang::PT:
      return "pt";
    case TtsLang::RO:
      return "ro";
    case TtsLang::RU:
      return "ru";
    case TtsLang::SK:
      return "sk";
    case TtsLang::SL:
      return "sl";
    case TtsLang::SV:
      return "sv";
    case TtsLang::TR:
      return "tr";
    case TtsLang::UK:
      return "uk";
    case TtsLang::VI:
      return "vi";
    case TtsLang::NA:
      return "na";
  }
  return "en";
}

enum class TtsQuality
{
  Auto,
  Low,
  Medium,
  High
};

const std::vector<std::string>& supportedLangCodes();

struct TtsRequest
{
  std::string text;
  TtsLang lang{TtsLang::EN};
  std::string voiceId{"M3"};
  TtsQuality quality{TtsQuality::Auto};
  float speed{1.05f};
};

using TtsChunkCallback =
    std::function<void(const std::vector<float>& chunkPcm)>;
