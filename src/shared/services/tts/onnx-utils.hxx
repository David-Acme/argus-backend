#pragma once

#include <cstdint>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>

class UnicodeProcessor;
class Style;

// --- Typed language enum ---
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

const std::vector<std::string>& supportedLangCodes();

struct Config
{
  struct AEConfig
  {
    int sampleRate;
    int baseChunkSize;
  } ae;

  struct TTLConfig
  {
    int chunkCompressFactor;
    int latentDim;
  } ttl;
};

// --- ONNX model loading ---
std::unique_ptr<Ort::Session> loadOnnx(Ort::Env& env, const std::string& path,
                                       const Ort::SessionOptions& opts);

struct OnnxModels
{
  std::unique_ptr<Ort::Session> dp;
  std::unique_ptr<Ort::Session> textEnc;
  std::unique_ptr<Ort::Session> vectorEst;
  std::unique_ptr<Ort::Session> vocoder;
};

OnnxModels loadOnnxAll(Ort::Env& env, const std::string& onnxDir,
                       const Ort::SessionOptions& opts);

// --- Config / processor loading ---
Config loadConfig(const std::string& onnxDir);
std::unique_ptr<UnicodeProcessor> loadProcessor(const std::string& onnxDir);

// --- Voice style loading ---
std::unique_ptr<Style> loadVoiceStyle(const std::string& path);

// --- Tensor conversion ---
Ort::Value
arrayToTensor(Ort::MemoryInfo& memoryInfo,
              const std::vector<std::vector<std::vector<float>>>& array,
              const std::vector<int64_t>& dims,
              std::vector<std::vector<float>>& bufferPool);

Ort::Value intArrayToTensor(Ort::MemoryInfo& memoryInfo,
                            const std::vector<std::vector<int64_t>>& array,
                            const std::vector<int64_t>& dims,
                            std::vector<std::vector<int64_t>>& bufferPool);

// --- Masks ---
std::vector<std::vector<std::vector<float>>>
lengthToMask(const std::vector<int64_t>& lengths, int maxLen = -1);

std::vector<std::vector<std::vector<float>>>
latentMask(const std::vector<int64_t>& wavLengths, int baseChunkSize,
           int chunkCompressFactor);

// --- WAV ---
void writeWav(const std::string& filename, const std::vector<float>& audioData,
              int sampleRate);

// --- JSON ---
std::vector<int64_t> loadJsonInt64(const std::string& path);

// --- Text ---
std::vector<std::string> chunkText(const std::string& text, int maxLen = 300);
std::string sanitizeFilename(const std::string& text, int maxLen);
