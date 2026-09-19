#include "onnx-utils.hxx"

#include "style.hxx"
#include "unicode-processor.hxx"

#include <algorithm>
#include <cctype>
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

std::unique_ptr<Ort::Session> loadOnnx(const LoadOnnxInput& input)
{
  return std::make_unique<Ort::Session>(input.env, input.path.c_str(),
                                        input.opts);
}

OnnxModels loadOnnxAll(const LoadOnnxAllInput& input)
{
  Ort::Env& env = input.env;
  const std::string& onnxDir = input.onnxDir;
  const Ort::SessionOptions& opts = input.opts;

  OnnxModels models;
  models.dp = loadOnnx({.env = env,
                        .path = onnxDir + "/duration_predictor.onnx",
                        .opts = opts});
  models.textEnc = loadOnnx(
      {.env = env, .path = onnxDir + "/text_encoder.onnx", .opts = opts});
  models.vectorEst = loadOnnx(
      {.env = env, .path = onnxDir + "/vector_estimator.onnx", .opts = opts});
  models.vocoder =
      loadOnnx({.env = env, .path = onnxDir + "/vocoder.onnx", .opts = opts});
  return models;
}

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

  return std::make_unique<Style>(
      StyleDeps{.ttlData = std::move(ttlFlat),
                .ttlShape = std::move(ttlShape),
                .dpData = std::move(dpFlat),
                .dpShape = std::move(dpShape)});
}

Ort::Value arrayToTensor(const ArrayToTensorInput& input)
{
  const std::vector<std::vector<std::vector<float>>>& array = input.array;
  const std::vector<int64_t>& dims = input.dims;
  std::vector<std::vector<float>>& bufferPool = input.bufferPool;

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

  return Ort::Value::CreateTensor<float>(input.memoryInfo, buffer.data(),
                                         buffer.size(), dims.data(),
                                         dims.size());
}

Ort::Value intArrayToTensor(const IntArrayToTensorInput& input)
{
  const std::vector<std::vector<int64_t>>& array = input.array;
  const std::vector<int64_t>& dims = input.dims;
  std::vector<std::vector<int64_t>>& bufferPool = input.bufferPool;

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

  return Ort::Value::CreateTensor<int64_t>(input.memoryInfo, buffer.data(),
                                           buffer.size(), dims.data(),
                                           dims.size());
}

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
latentMask(const LatentMaskInput& input)
{
  const std::vector<int64_t>& wavLengths = input.wavLengths;
  const int baseChunkSize = input.baseChunkSize;
  const int chunkCompressFactor = input.chunkCompressFactor;

  int latentSize = baseChunkSize * chunkCompressFactor;
  std::vector<int64_t> latentLengths;
  latentLengths.reserve(wavLengths.size());
  for (auto len : wavLengths) {
    latentLengths.push_back((len + latentSize - 1) / latentSize);
  }
  return lengthToMask(latentLengths);
}

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


namespace
{

bool isSentenceEnd(const std::string& text, size_t dot)
{
  const char c = text[dot];
  if (c == '!' || c == '?')
    return true;
  if (c != '.')
    return false;

  if (dot + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[dot + 1])))
    return false;
  if (dot > 0 && std::isdigit(static_cast<unsigned char>(text[dot - 1])))
    return false;

  size_t begin = dot;
  while (begin > 0) {
    const auto prev = static_cast<unsigned char>(text[begin - 1]);
    if (std::isspace(prev) || prev == '.')
      break;
    --begin;
  }
  std::string word = text.substr(begin, dot - begin);
  for (auto& ch : word)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  static const std::vector<std::string> kAbbrev = {
      "sr",  "sra", "srta", "dr",   "dra", "ud",  "uds", "etc", "vs",
      "ej",  "av",  "pag",  "num",  "tel", "min", "max", "aprox",
      "mr",  "mrs", "ms",   "prof", "st",  "no",  "vol", "fig"};
  for (const auto& a : kAbbrev)
    if (word == a)
      return false;

  if (word.size() == 1 && std::isalpha(static_cast<unsigned char>(word[0])))
    return false;

  return true;
}

std::vector<std::string> splitSentences(const std::string& paragraph)
{
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i < paragraph.size(); ++i) {
    const char c = paragraph[i];
    if (c != '.' && c != '!' && c != '?')
      continue;
    size_t after = i + 1;
    while (after < paragraph.size() &&
           (paragraph[after] == '.' || paragraph[after] == '!' ||
            paragraph[after] == '?' || paragraph[after] == '"' ||
            paragraph[after] == '\''))
      ++after;
    if (after >= paragraph.size())
      break;
    if (!std::isspace(static_cast<unsigned char>(paragraph[after])))
      continue;
    if (!isSentenceEnd(paragraph, i))
      continue;
    size_t next = after;
    while (next < paragraph.size() &&
           std::isspace(static_cast<unsigned char>(paragraph[next])))
      ++next;
    out.push_back(paragraph.substr(start, next - start));
    start = next;
    i = next - 1;
  }
  if (start < paragraph.size()) {
    std::string tail = paragraph.substr(start);
    if (!trim(tail).empty())
      out.push_back(std::move(tail));
  }
  return out;
}

} // namespace

size_t completeSentenceEnd(const std::string& buffer, size_t minChars)
{
  for (size_t i = 0; i < buffer.size(); ++i) {
    const char c = buffer[i];
    if (c != '.' && c != '!' && c != '?')
      continue;
    size_t after = i + 1;
    while (after < buffer.size() &&
           (buffer[after] == '.' || buffer[after] == '!' ||
            buffer[after] == '?' || buffer[after] == '"' ||
            buffer[after] == '\''))
      ++after;
    if (after >= buffer.size())
      return 0;
    if (!std::isspace(static_cast<unsigned char>(buffer[after])))
      continue;
    if (!isSentenceEnd(buffer, i))
      continue;
    size_t next = after;
    while (next < buffer.size() &&
           std::isspace(static_cast<unsigned char>(buffer[next])))
      ++next;
    if (next >= buffer.size())
      return 0;
    if (next < minChars)
      continue;
    return next;
  }
  return 0;
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
    std::vector<std::string> sentences = splitSentences(paragraph);

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
