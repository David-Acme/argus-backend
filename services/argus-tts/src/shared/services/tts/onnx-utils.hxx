#pragma once

#include <shared/services/tts/tts-wire.hxx>

#include <memory>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>

class UnicodeProcessor;
class Style;

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

struct LoadOnnxInput
{
  Ort::Env& env;
  const std::string& path;
  const Ort::SessionOptions& opts;
};

std::unique_ptr<Ort::Session> loadOnnx(const LoadOnnxInput& input);

struct OnnxModels
{
  std::unique_ptr<Ort::Session> dp;
  std::unique_ptr<Ort::Session> textEnc;
  std::unique_ptr<Ort::Session> vectorEst;
  std::unique_ptr<Ort::Session> vocoder;
};

struct LoadOnnxAllInput
{
  Ort::Env& env;
  const std::string& onnxDir;
  const Ort::SessionOptions& opts;
};

OnnxModels loadOnnxAll(const LoadOnnxAllInput& input);

Config loadConfig(const std::string& onnxDir);
std::unique_ptr<UnicodeProcessor> loadProcessor(const std::string& onnxDir);

std::unique_ptr<Style> loadVoiceStyle(const std::string& path);

struct ArrayToTensorInput
{
  Ort::MemoryInfo& memoryInfo;
  const std::vector<std::vector<std::vector<float>>>& array;
  const std::vector<int64_t>& dims;
  std::vector<std::vector<float>>& bufferPool;
};

Ort::Value arrayToTensor(const ArrayToTensorInput& input);

struct IntArrayToTensorInput
{
  Ort::MemoryInfo& memoryInfo;
  const std::vector<std::vector<int64_t>>& array;
  const std::vector<int64_t>& dims;
  std::vector<std::vector<int64_t>>& bufferPool;
};

Ort::Value intArrayToTensor(const IntArrayToTensorInput& input);

std::vector<std::vector<std::vector<float>>>
lengthToMask(const std::vector<int64_t>& lengths, int maxLen = -1);

struct LatentMaskInput
{
  const std::vector<int64_t>& wavLengths;
  int baseChunkSize{0};
  int chunkCompressFactor{0};
};

std::vector<std::vector<std::vector<float>>>
latentMask(const LatentMaskInput& input);

std::vector<int64_t> loadJsonInt64(const std::string& path);

std::vector<std::string> chunkText(const std::string& text, int maxLen = 300);
size_t completeSentenceEnd(const std::string& buffer, size_t minChars = 0);
std::string sanitizeFilename(const std::string& text, int maxLen);
