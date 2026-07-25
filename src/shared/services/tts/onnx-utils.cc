#include "onnx-utils.hxx"

#include "style.hxx"
#include "unicode-processor.hxx"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>

using json = nlohmann::json;

const std::vector<std::string>& supportedLangCodes()
{
  static const std::vector<std::string> codes = {"en", "ko", "ja", "ar", "bg",
                                                 "cs", "da", "de", "el", "es",
                                                 "et", "fi", "fr", "hi", "hr",
                                                 "hu", "id", "it", "lt", "lv",
                                                 "nl", "pl", "pt", "ro", "ru",
                                                 "sk", "sl", "sv", "tr", "uk",
                                                 "vi", "na"};
  return codes;
}

// ============================================================================
// ONNX model loading
// ============================================================================

std::unique_ptr<Ort::Session> loadOnnx(Ort::Env& env, const std::string& path,
                                       const Ort::SessionOptions& opts)
{
  return std::make_unique<Ort::Session>(env, path.c_str(), opts);
}

OnnxModels loadOnnxAll(Ort::Env& env, const std::string& onnxDir,
                       const Ort::SessionOptions& opts)
{
  OnnxModels models;
  models.dp = loadOnnx(env, onnxDir + "/duration_predictor.onnx", opts);
  models.textEnc = loadOnnx(env, onnxDir + "/text_encoder.onnx", opts);
  models.vectorEst = loadOnnx(env, onnxDir + "/vector_estimator.onnx", opts);
  models.vocoder = loadOnnx(env, onnxDir + "/vocoder.onnx", opts);
  return models;
}

// ============================================================================
// Config / processor loading
// ============================================================================

Config loadConfig(const std::string& onnxDir)
{
  std::string cfgPath = onnxDir + "/tts.json";
  std::ifstream file(cfgPath);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open config file: " + cfgPath);
  }

  json j;
  file >> j;

  Config cfg;
  cfg.ae.sampleRate = j["ae"]["sample_rate"];
  cfg.ae.baseChunkSize = j["ae"]["base_chunk_size"];
  cfg.ttl.chunkCompressFactor = j["ttl"]["chunk_compress_factor"];
  cfg.ttl.latentDim = j["ttl"]["latent_dim"];

  return cfg;
}

std::unique_ptr<UnicodeProcessor> loadProcessor(const std::string& onnxDir)
{
  std::string path = onnxDir + "/unicode_indexer.json";
  return std::make_unique<UnicodeProcessor>(path);
}

// ============================================================================
// Voice style loading
// ============================================================================

std::unique_ptr<Style> loadVoiceStyle(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open voice style file: " + path);
  }

  json j;
  file >> j;

  auto ttlDims = j["style_ttl"]["dims"].get<std::vector<int64_t>>();
  auto dpDims = j["style_dp"]["dims"].get<std::vector<int64_t>>();

  int64_t ttlDim1 = ttlDims[1];
  int64_t ttlDim2 = ttlDims[2];
  int64_t dpDim1 = dpDims[1];
  int64_t dpDim2 = dpDims[2];

  std::vector<float> ttlFlat;
  ttlFlat.reserve(ttlDim1 * ttlDim2);
  auto ttlData = j["style_ttl"]["data"]
                     .get<std::vector<std::vector<std::vector<float>>>>();
  for (const auto& batch : ttlData) {
    for (const auto& row : batch) {
      ttlFlat.insert(ttlFlat.end(), row.begin(), row.end());
    }
  }

  std::vector<float> dpFlat;
  dpFlat.reserve(dpDim1 * dpDim2);
  auto dpData =
      j["style_dp"]["data"].get<std::vector<std::vector<std::vector<float>>>>();
  for (const auto& batch : dpData) {
    for (const auto& row : batch) {
      dpFlat.insert(dpFlat.end(), row.begin(), row.end());
    }
  }

  std::vector<int64_t> ttlShape = {1, ttlDim1, ttlDim2};
  std::vector<int64_t> dpShape = {1, dpDim1, dpDim2};

  return std::make_unique<Style>(std::move(ttlFlat), std::move(ttlShape),
                                 std::move(dpFlat), std::move(dpShape));
}

// ============================================================================
// Tensor conversion
// ============================================================================

Ort::Value
arrayToTensor(Ort::MemoryInfo& memoryInfo,
              const std::vector<std::vector<std::vector<float>>>& array,
              const std::vector<int64_t>& dims,
              std::vector<std::vector<float>>& bufferPool)
{
  size_t total = 1;
  for (auto d : dims)
    total *= d;

  std::vector<float> flat;
  flat.reserve(total);
  for (const auto& batch : array) {
    for (const auto& row : batch) {
      for (float val : row) {
        flat.push_back(val);
      }
    }
  }

  bufferPool.push_back(std::move(flat));
  auto& buffer = bufferPool.back();

  return Ort::Value::CreateTensor<float>(memoryInfo, buffer.data(),
                                         buffer.size(), dims.data(),
                                         dims.size());
}

Ort::Value intArrayToTensor(Ort::MemoryInfo& memoryInfo,
                            const std::vector<std::vector<int64_t>>& array,
                            const std::vector<int64_t>& dims,
                            std::vector<std::vector<int64_t>>& bufferPool)
{
  size_t total = 1;
  for (auto d : dims)
    total *= d;

  std::vector<int64_t> flat;
  flat.reserve(total);
  for (const auto& row : array) {
    for (int64_t val : row) {
      flat.push_back(val);
    }
  }

  bufferPool.push_back(std::move(flat));
  auto& buffer = bufferPool.back();

  return Ort::Value::CreateTensor<int64_t>(memoryInfo, buffer.data(),
                                           buffer.size(), dims.data(),
                                           dims.size());
}

// ============================================================================
// Masks
// ============================================================================

std::vector<std::vector<std::vector<float>>>
lengthToMask(const std::vector<int64_t>& lengths, int maxLen)
{
  if (maxLen == -1) {
    maxLen = *std::max_element(lengths.begin(), lengths.end());
  }

  std::vector<std::vector<std::vector<float>>> mask;
  mask.reserve(lengths.size());
  for (auto len : lengths) {
    std::vector<std::vector<float>> batchMask(1);
    batchMask[0].resize(maxLen);
    for (int i = 0; i < maxLen; i++) {
      batchMask[0][i] = (i < len) ? 1.0f : 0.0f;
    }
    mask.push_back(std::move(batchMask));
  }
  return mask;
}

std::vector<std::vector<std::vector<float>>>
latentMask(const std::vector<int64_t>& wavLengths, int baseChunkSize,
           int chunkCompressFactor)
{
  int latentSize = baseChunkSize * chunkCompressFactor;
  std::vector<int64_t> latentLengths;
  latentLengths.reserve(wavLengths.size());
  for (auto len : wavLengths) {
    latentLengths.push_back((len + latentSize - 1) / latentSize);
  }
  return lengthToMask(latentLengths);
}

// ============================================================================
// WAV
// ============================================================================

void writeWav(const std::string& filename, const std::vector<float>& audioData,
              int sampleRate)
{
  std::ofstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file for writing: " + filename);
  }

  int numChannels = 1;
  int bitsPerSample = 16;
  int byteRate = sampleRate * numChannels * bitsPerSample / 8;
  int blockAlign = numChannels * bitsPerSample / 8;
  int dataSize = audioData.size() * bitsPerSample / 8;

  file.write("RIFF", 4);
  int32_t chunkSize = 36 + dataSize;
  file.write(reinterpret_cast<const char*>(&chunkSize), 4);
  file.write("WAVE", 4);

  file.write("fmt ", 4);
  int32_t fmtChunkSize = 16;
  file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
  int16_t audioFormat = 1;
  file.write(reinterpret_cast<const char*>(&audioFormat), 2);
  int16_t numChannels16 = static_cast<int16_t>(numChannels);
  file.write(reinterpret_cast<const char*>(&numChannels16), 2);
  file.write(reinterpret_cast<const char*>(&sampleRate), 4);
  file.write(reinterpret_cast<const char*>(&byteRate), 4);
  int16_t blockAlign16 = static_cast<int16_t>(blockAlign);
  file.write(reinterpret_cast<const char*>(&blockAlign16), 2);
  int16_t bitsPerSample16 = static_cast<int16_t>(bitsPerSample);
  file.write(reinterpret_cast<const char*>(&bitsPerSample16), 2);

  file.write("data", 4);
  file.write(reinterpret_cast<const char*>(&dataSize), 4);

  std::vector<int16_t> intSamples;
  intSamples.reserve(audioData.size());
  for (float sample : audioData) {
    float clamped = std::max(-1.0f, std::min(1.0f, sample));
    intSamples.push_back(static_cast<int16_t>(clamped * 32767));
  }

  file.write(reinterpret_cast<const char*>(intSamples.data()),
             intSamples.size() * sizeof(int16_t));
}

// ============================================================================
// JSON
// ============================================================================

std::vector<int64_t> loadJsonInt64(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open file: " + path);
  }

  json j;
  file >> j;
  return j.get<std::vector<int64_t>>();
}

// ============================================================================
// Text utilities
// ============================================================================

static std::string trim(const std::string& str)
{
  size_t start = 0;
  while (start < str.size() &&
         std::isspace(static_cast<unsigned char>(str[start]))) {
    start++;
  }

  size_t end = str.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(str[end - 1]))) {
    end--;
  }

  return str.substr(start, end - start);
}

std::vector<std::string> chunkText(const std::string& text, int maxLen)
{
  std::vector<std::string> chunks;

  static const std::regex paragraphRe(R"(\n\s*\n+)");
  static const std::regex sentenceRe(R"([.!?]\s+)");

  std::vector<std::string> paragraphs;
  {
    std::sregex_token_iterator iter(text.begin(), text.end(), paragraphRe, -1);
    std::sregex_token_iterator end;
    for (; iter != end; ++iter) {
      std::string para = trim(*iter);
      if (!para.empty()) {
        paragraphs.push_back(std::move(para));
      }
    }
  }

  for (const auto& paragraph : paragraphs) {
    std::sregex_token_iterator sentIter(paragraph.begin(), paragraph.end(),
                                        sentenceRe, -1);
    std::sregex_token_iterator sentEnd;

    std::vector<std::string> sentences;
    for (; sentIter != sentEnd; ++sentIter) {
      std::string sentence = *sentIter;
      if (!sentence.empty()) {
        std::smatch match;
        if (std::regex_search(sentIter->first, paragraph.end(), match,
                              sentenceRe)) {
          sentence += match.str();
        }
        sentences.push_back(sentence);
      }
    }

    std::string currentChunk;
    for (const auto& sentence : sentences) {
      if (static_cast<int>(currentChunk.length() + sentence.length() + 1) <=
          maxLen) {
        if (!currentChunk.empty()) {
          currentChunk += " ";
        }
        currentChunk += sentence;
      }
      else {
        if (!currentChunk.empty()) {
          chunks.push_back(trim(currentChunk));
        }
        currentChunk = sentence;
      }
    }

    if (!currentChunk.empty()) {
      chunks.push_back(trim(currentChunk));
    }
  }

  if (chunks.empty()) {
    chunks.push_back(trim(text));
  }

  return chunks;
}

std::string sanitizeFilename(const std::string& text, int maxLen)
{
  std::string result;
  int charCount = 0;
  size_t i = 0;

  while (i < text.size() && charCount < maxLen) {
    unsigned char c = static_cast<unsigned char>(text[i]);

    if (std::isalnum(c) || c == '_') {
      result += text[i];
      i++;
      charCount++;
    }
    else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      result += text.substr(i, 2);
      i += 2;
      charCount++;
    }
    else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      result += text.substr(i, 3);
      i += 3;
      charCount++;
    }
    else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
      result += text.substr(i, 4);
      i += 4;
      charCount++;
    }
    else {
      result += '_';
      i++;
      charCount++;
    }
  }
  return result;
}
