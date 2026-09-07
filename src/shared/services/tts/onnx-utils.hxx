#pragma once

#include "tts-wire.hxx"

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
size_t completeSentenceEnd(const std::string& buffer, size_t minChars = 0);
std::string sanitizeFilename(const std::string& text, int maxLen);
