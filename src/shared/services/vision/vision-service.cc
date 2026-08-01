#include "vision-service.hxx"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <drogon/drogon.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <vector>

using nlohmann::json;

namespace
{

constexpr int kImageSize = 512;
constexpr int64_t kEosId = 49279;
constexpr int64_t kImageTokenId = 49190;
constexpr int64_t kFakeAroundId = 49189;
constexpr int64_t kGlobalImgId = 49152;
constexpr int kNumImageTokens = 64;
constexpr int kNumLayers = 32;
constexpr int kNumKvHeads = 5;
constexpr int kHeadDim = 64;
constexpr int kEmbedDim = 960;
constexpr int kNumDecoderOutputs = 1 + 2 * kNumLayers;

const std::string kModelDir = "models/vision/smolvlm";
const std::string kVisionEncoderPath = kModelDir + "/vision_encoder_int8.onnx";
const std::string kDecoderMergedPath =
    kModelDir + "/decoder_model_merged_int8.onnx";
const std::string kEmbedTokensPath = kModelDir + "/embed_tokens_int8.onnx";
const std::string kTokenizerPath = kModelDir + "/tokenizer.json";

const float kMean[3] = {0.5F, 0.5F, 0.5F};
const float kStd[3] = {0.5F, 0.5F, 0.5F};

Ort::MemoryInfo& cpuMem()
{
  static Ort::MemoryInfo info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return info;
}

// GPT-2 byte-level BPE byte decoder: unicode codepoint -> byte.
uint8_t byteDecodeTable(uint32_t cp)
{
  if (cp >= 0x21 && cp <= 0x7E)
    return static_cast<uint8_t>(cp);
  if (cp >= 0xA1 && cp <= 0xAC)
    return static_cast<uint8_t>(cp);
  if (cp >= 0xAE && cp <= 0xFF)
    return static_cast<uint8_t>(cp);
  if (cp >= 0x100 && cp <= 0x143)
    return static_cast<uint8_t>(cp - 0x100);
  return 0;
}

std::string utf8Encode(uint32_t cp)
{
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  }
  else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

// Byte-level BPE detokenization: codepoints 0x100-0x143 map back to raw
// bytes (U+0120 = space); the rest is passed through as UTF-8.
std::string byteDecode(const std::string& token)
{
  std::string out;
  size_t i = 0;
  while (i < token.size()) {
    const uint8_t lead = static_cast<uint8_t>(token[i]);
    uint32_t cp = 0;
    int extra = 0;
    if (lead < 0x80) {
      cp = lead;
    }
    else if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      extra = 1;
    }
    else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      extra = 2;
    }
    else {
      cp = lead & 0x07;
      extra = 3;
    }
    for (int j = 1; j <= extra && i + j < token.size(); ++j)
      cp = (cp << 6) | (static_cast<uint8_t>(token[i + j]) & 0x3F);
    i += extra + 1;
    const uint8_t b = byteDecodeTable(cp);
    if (b != 0)
      out.push_back(static_cast<char>(b));
    else
      out += utf8Encode(cp);
  }
  return out;
}

} // namespace

std::unique_ptr<Ort::Session> VisionService::visionEncoder_;
std::unique_ptr<Ort::Session> VisionService::decoderMerged_;
std::unique_ptr<Ort::Session> VisionService::embedTokens_;
Ort::Env VisionService::env_{ORT_LOGGING_LEVEL_ERROR, "Argus-Vision"};
std::mutex VisionService::mutex_;
bool VisionService::loaded_ = false;
int32_t VisionService::defaultMaxTokens_ = 64;
VisionService::FrameCache VisionService::cacheA_;
VisionService::FrameCache VisionService::cacheB_;
int VisionService::lastCacheSlot_ = 0;
std::vector<std::string> VisionService::idToToken_;
std::unordered_set<int64_t> VisionService::specialIds_;

const std::vector<int64_t>& VisionService::promptIds()
{
  // SmolVLM2 chat template with one expanded <image> block and a fixed
  // captioning instruction:
  //   <|im_start|>User:<image>Can you describe this image?
  //   <end_of_utterance>\nAssistant:
  // Tokenized with the model's GPT-2 byte-level BPE tokenizer (ids verified
  // against transformers; the 64 <image> slots are filled at runtime by the
  // vision encoder features via inputs_merger).
  static const std::vector<int64_t> ids = {
      1,      11126,  42,     kFakeAroundId, kGlobalImgId,
      // 64 x <image>
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kImageTokenId, kImageTokenId, kImageTokenId, kImageTokenId,
      kFakeAroundId, 7306, 346, 5125, 451, 2443, 47, 49279,
      198, 9519, 9531, 42,
  };
  return ids;
}

void VisionService::init()
{
  try {
    auto opts = Ort::SessionOptions{};
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    int intraThreads = ThreadBudget::computeThreads();
    if (const int cfg = ConfigService::getInt("vision.threads"); cfg > 0)
      intraThreads = cfg;
    opts.SetIntraOpNumThreads(intraThreads);
    opts.SetInterOpNumThreads(1);
    opts.AddConfigEntry("session.intra_op.spin_duration_us", "1000");
    opts.AddConfigEntry("session.intra_op.spin_backoff_max", "8");

    visionEncoder_ = std::make_unique<Ort::Session>(
        env_, kVisionEncoderPath.c_str(), opts);
    decoderMerged_ = std::make_unique<Ort::Session>(
        env_, kDecoderMergedPath.c_str(), opts);
    embedTokens_ = std::make_unique<Ort::Session>(
        env_, kEmbedTokensPath.c_str(), opts);

    std::ifstream tokFile(kTokenizerPath);
    if (!tokFile.is_open())
      throw std::runtime_error("failed to open tokenizer: " + kTokenizerPath);
    json tok;
    tokFile >> tok;

    const size_t vocabSize = 49280;
    idToToken_.assign(vocabSize, "");
    for (auto it = tok.at("model").at("vocab").begin();
         it != tok.at("model").at("vocab").end(); ++it) {
      const int64_t id = it.value().get<int64_t>();
      if (id >= 0 && id < static_cast<int64_t>(idToToken_.size()))
        idToToken_[static_cast<size_t>(id)] = it.key();
    }
    for (const auto& added : tok.at("added_tokens")) {
      const int64_t id = added.at("id").get<int64_t>();
      if (id >= 0 && id < static_cast<int64_t>(idToToken_.size()))
        idToToken_[static_cast<size_t>(id)] =
            added.at("content").get<std::string>();
      specialIds_.insert(id);
    }

    loaded_ = true;
    defaultMaxTokens_ =
        std::clamp<int32_t>(ConfigService::getInt("vision.max_tokens"), 8, 512);
    LOG_INFO << "Vision loaded: SmolVLM2-500M-Video-Instruct (ONNX int8, threads="
             << intraThreads << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "Vision init failed: " << e.what();
    shutdown();
  }
}

void VisionService::shutdown()
{
  embedTokens_.reset();
  decoderMerged_.reset();
  visionEncoder_.reset();
  idToToken_.clear();
  specialIds_.clear();
  loaded_ = false;
  LOG_INFO << "Vision shutdown";
}

bool VisionService::isLoaded()
{
  return loaded_;
}

std::vector<float> VisionService::preprocess(const unsigned char* rgb,
                                             int width, int height)
{
  cv::Mat src(height, width, CV_8UC3, const_cast<unsigned char*>(rgb));
  cv::Mat resized;
  cv::resize(src, resized, cv::Size(kImageSize, kImageSize), 0, 0,
             cv::INTER_LANCZOS4);

  std::vector<float> pixels(static_cast<size_t>(3) * kImageSize * kImageSize);
  const float scale = 1.0F / 255.0F;
  size_t idx = 0;
  for (int c = 0; c < 3; ++c) {
    const float mean = kMean[c];
    const float invStd = 1.0F / kStd[c];
    for (int y = 0; y < kImageSize; ++y) {
      const uint8_t* row = resized.ptr<uint8_t>(y);
      for (int x = 0; x < kImageSize; ++x)
        pixels[idx++] = (row[x * 3 + c] * scale - mean) * invStd;
    }
  }
  return pixels;
}

std::vector<float> VisionService::embed(int64_t token)
{
  int64_t ids[1] = {token};
  const std::vector<int64_t> shape = {1, 1};
  auto input = Ort::Value::CreateTensor<int64_t>(cpuMem(), ids, 1, shape.data(),
                                                 shape.size());
  const char* inputNames[] = {"input_ids"};
  const char* outputNames[] = {"inputs_embeds"};
  auto out = embedTokens_->Run(Ort::RunOptions{}, inputNames, &input, 1,
                               outputNames, 1);
  const auto outShape = out[0].GetTensorTypeAndShapeInfo().GetShape();
  const size_t n = static_cast<size_t>(outShape[1]) * kEmbedDim;
  const float* data = out[0].GetTensorData<float>();
  return std::vector<float>(data, data + n);
}

std::vector<std::string> VisionService::decodeTokens(
    const std::vector<int64_t>& ids)
{
  std::vector<std::string> words;
  for (int64_t id : ids) {
    if (id == kEosId || id == 0)
      continue;
    if (id < 0 || id >= static_cast<int64_t>(idToToken_.size()))
      continue;
    if (specialIds_.count(id) > 0)
      continue;
    const auto& token = idToToken_[static_cast<size_t>(id)];
    if (token.empty())
      continue;
    words.push_back(byteDecode(token));
  }
  return words;
}

std::string VisionService::describe(const VisionRequest& req)
{
  if (!loaded_) {
    LOG_WARN << "Vision: service not loaded";
    return "";
  }

  std::lock_guard<std::mutex> lock(mutex_);

  const int32_t maxTokens =
      req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;

  try {
    auto pixels = preprocess(req.imageRgb.data(),
                             static_cast<int>(req.width),
                             static_cast<int>(req.height));

    // Frame cache: identical frames skip the (expensive) vision encoder.
    uint64_t hash = 14695981039346656037ULL;
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(pixels.data());
    for (size_t i = 0; i < pixels.size() * sizeof(float); ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ULL;
    }

    std::vector<float> imageFeatures;
    FrameCache* hit = nullptr;
    if (cacheA_.hash == hash && cacheA_.numTokens > 0) {
      hit = &cacheA_;
    }
    else if (cacheB_.hash == hash && cacheB_.numTokens > 0) {
      hit = &cacheB_;
    }
    if (hit) {
      imageFeatures = hit->features;
    }
    else {
      const char* visionIn[] = {"pixel_values", "pixel_attention_mask"};
      const char* visionOut[] = {"image_features"};
      const std::vector<int64_t> pixelShape = {1, 1, 3, kImageSize,
                                               kImageSize};
      const std::vector<int64_t> maskShape = {1, 1, kImageSize, kImageSize};
      auto maskData =
          std::make_unique<bool[]>(static_cast<size_t>(kImageSize) *
                                   kImageSize);
      std::fill(maskData.get(), maskData.get() +
                                    static_cast<size_t>(kImageSize) *
                                        kImageSize,
                true);
      std::vector<Ort::Value> visionFeeds;
      visionFeeds.push_back(Ort::Value::CreateTensor<float>(
          cpuMem(), pixels.data(), pixels.size(), pixelShape.data(),
          pixelShape.size()));
      visionFeeds.push_back(Ort::Value::CreateTensor<bool>(
          cpuMem(), maskData.get(),
          static_cast<size_t>(kImageSize) * kImageSize, maskShape.data(),
          maskShape.size()));
      auto visionOuts = visionEncoder_->Run(Ort::RunOptions{}, visionIn,
                                            visionFeeds.data(),
                                            visionFeeds.size(), visionOut, 1);
      const auto imageShape =
          visionOuts[0].GetTensorTypeAndShapeInfo().GetShape();
      const int64_t actualTokens = imageShape[1];
      const size_t imageFeatCount =
          static_cast<size_t>(actualTokens) * kEmbedDim;
      imageFeatures.assign(visionOuts[0].GetTensorData<float>(),
                           visionOuts[0].GetTensorData<float>() +
                               imageFeatCount);
      FrameCache* slot = lastCacheSlot_ == 1 ? &cacheA_ : &cacheB_;
      slot->hash = hash;
      slot->numTokens = actualTokens;
      slot->features = imageFeatures;
      lastCacheSlot_ = lastCacheSlot_ == 1 ? 2 : 1;
    }

    // Prefill: build the prompt embeddings and inject the image features into
    // the <image> token rows.
    const auto& prompt = promptIds();
    const std::vector<int64_t> promptShape = {
        1, static_cast<int64_t>(prompt.size())};
    auto promptInput = Ort::Value::CreateTensor<int64_t>(
        cpuMem(), const_cast<int64_t*>(prompt.data()), prompt.size(),
        promptShape.data(), promptShape.size());
    const char* embedIn[] = {"input_ids"};
    const char* embedOut[] = {"inputs_embeds"};
    auto textOuts = embedTokens_->Run(Ort::RunOptions{}, embedIn,
                                      &promptInput, 1, embedOut, 1);

    const int64_t promptLen = static_cast<int64_t>(prompt.size());
    std::vector<float> textEmbeds(
        textOuts[0].GetTensorData<float>(),
        textOuts[0].GetTensorData<float>() +
            static_cast<size_t>(promptLen) * kEmbedDim);

    // Row index where the 64 <image> tokens start (after the 3-token BOS
    // prefix + fake/global markers).
    const int64_t imageStart = 5;
    std::vector<float> encEmbeds(textEmbeds);
    std::memcpy(encEmbeds.data() + imageStart * kEmbedDim,
                imageFeatures.data(),
                static_cast<size_t>(kNumImageTokens) * kEmbedDim *
                    sizeof(float));

    const int64_t totalLen = promptLen;
    std::vector<int64_t> attnData(static_cast<size_t>(totalLen), 1);
    const std::vector<int64_t> attnShape = {1, totalLen};
    std::vector<int64_t> posData(static_cast<size_t>(totalLen));
    for (int64_t i = 0; i < totalLen; ++i)
      posData[static_cast<size_t>(i)] = i;
    const std::vector<int64_t> posShape = {1, totalLen};
    const std::vector<int64_t> embShape = {1, totalLen, kEmbedDim};

    // First decoder run: empty KV cache.
    static const std::vector<float> kEmptyPast(1, 0.0F);
    const std::vector<int64_t> emptyPastShape = {1, kNumKvHeads, 0, kHeadDim};
    std::vector<Ort::Value> feeds;
    std::vector<std::string> names;
    feeds.push_back(Ort::Value::CreateTensor<float>(
        cpuMem(), encEmbeds.data(), encEmbeds.size(), embShape.data(),
        embShape.size()));
    names.emplace_back("inputs_embeds");
    feeds.push_back(Ort::Value::CreateTensor<int64_t>(
        cpuMem(), attnData.data(), attnData.size(), attnShape.data(),
        attnShape.size()));
    names.emplace_back("attention_mask");
    feeds.push_back(Ort::Value::CreateTensor<int64_t>(
        cpuMem(), posData.data(), posData.size(), posShape.data(),
        posShape.size()));
    names.emplace_back("position_ids");

    const char* decoderOutputNames[kNumDecoderOutputs];
    decoderOutputNames[0] = "logits";
    for (int l = 0; l < kNumLayers; ++l) {
      static std::vector<std::string> namesStorage(2 * kNumLayers);
      namesStorage[static_cast<size_t>(2 * l)] =
          "present." + std::to_string(l) + ".key";
      namesStorage[static_cast<size_t>(2 * l + 1)] =
          "present." + std::to_string(l) + ".value";
      decoderOutputNames[1 + 2 * l] = namesStorage[static_cast<size_t>(2 * l)].c_str();
      decoderOutputNames[2 + 2 * l] =
          namesStorage[static_cast<size_t>(2 * l + 1)].c_str();
    }

    // past_key_values inputs (empty on first step).
    for (int l = 0; l < kNumLayers; ++l) {
      for (int kv = 0; kv < 2; ++kv) {
        names.push_back("past_key_values." + std::to_string(l) + "." +
                        (kv == 0 ? "key" : "value"));
        feeds.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), const_cast<float*>(kEmptyPast.data()), kEmptyPast.size(),
            emptyPastShape.data(), emptyPastShape.size()));
      }
    }

    std::vector<const char*> inputNames;
    inputNames.reserve(names.size());
    for (const auto& n : names)
      inputNames.push_back(n.c_str());

    auto firstOuts = decoderMerged_->Run(
        Ort::RunOptions{}, inputNames.data(), feeds.data(), feeds.size(),
        decoderOutputNames, kNumDecoderOutputs);

    // Keep KV cache as a growing vector of floats per layer.
    std::vector<std::vector<float>> pastKeys(kNumLayers), pastValues(kNumLayers);
    std::vector<int64_t> pastShapes(static_cast<size_t>(2 * kNumLayers), 0);
    auto copyPast = [&](const std::vector<Ort::Value>& outs, int outIdx, int l,
                        bool key) {
      auto& dst = key ? pastKeys[static_cast<size_t>(l)]
                      : pastValues[static_cast<size_t>(l)];
      const auto sh = outs[static_cast<size_t>(outIdx)]
                          .GetTensorTypeAndShapeInfo()
                          .GetShape();
      dst.assign(outs[static_cast<size_t>(outIdx)].GetTensorData<float>(),
                 outs[static_cast<size_t>(outIdx)].GetTensorData<float>() +
                     static_cast<size_t>(sh[0] * sh[1] * sh[2] * sh[3]));
      pastShapes[static_cast<size_t>(2 * l + (key ? 0 : 1))] = sh[2];
    };
    for (int l = 0; l < kNumLayers; ++l) {
      copyPast(firstOuts, 1 + 2 * l, l, true);
      copyPast(firstOuts, 2 + 2 * l, l, false);
    }

    // Greedy decode.
    auto pickBest = [&](const std::vector<Ort::Value>& outs) {
      const auto logitsShape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
      const int64_t vocab = logitsShape[2];
      const int64_t lastPos = logitsShape[1] - 1;
      const auto* logitsBase = outs[0].GetTensorData<float>();
      const auto* logitsRow =
          logitsBase + static_cast<size_t>(lastPos) * vocab;
      int64_t best = 0;
      float bestScore = -1e30F;
      for (int64_t v = 0; v < vocab; ++v) {
        const float s = logitsRow[static_cast<size_t>(v)];
        if (s > bestScore) {
          bestScore = s;
          best = v;
        }
      }
      return best;
    };

    std::vector<int64_t> generated;
    std::vector<int64_t> attnDataRunning(static_cast<size_t>(totalLen), 1);
    int64_t curLen = totalLen;
    auto runDecoderStep = [&](int64_t token, int64_t step) {
      auto emb = embed(token);
      const std::vector<int64_t> emb1Shape = {1, 1, kEmbedDim};
      const std::vector<int64_t> attn1Shape = {1, curLen + 1};
      const std::vector<int64_t> pos1Shape = {1, 1};
      attnDataRunning.push_back(1);
      std::vector<int64_t> pos1 = {step};

      std::vector<Ort::Value> feeds2;
      std::vector<std::string> names2;
      feeds2.push_back(Ort::Value::CreateTensor<float>(
          cpuMem(), emb.data(), emb.size(), emb1Shape.data(), emb1Shape.size()));
      names2.emplace_back("inputs_embeds");
      feeds2.push_back(Ort::Value::CreateTensor<int64_t>(
          cpuMem(), attnDataRunning.data(), attnDataRunning.size(),
          attn1Shape.data(), attn1Shape.size()));
      names2.emplace_back("attention_mask");
      feeds2.push_back(Ort::Value::CreateTensor<int64_t>(
          cpuMem(), pos1.data(), pos1.size(), pos1Shape.data(),
          pos1Shape.size()));
      names2.emplace_back("position_ids");

      for (int l = 0; l < kNumLayers; ++l) {
        const int64_t pastLen = pastShapes[static_cast<size_t>(2 * l)];
        const std::vector<int64_t> pastShape = {1, kNumKvHeads, pastLen,
                                                kHeadDim};
        feeds2.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), pastKeys[static_cast<size_t>(l)].data(),
            pastKeys[static_cast<size_t>(l)].size(), pastShape.data(),
            pastShape.size()));
        names2.emplace_back("past_key_values." + std::to_string(l) + ".key");
        feeds2.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), pastValues[static_cast<size_t>(l)].data(),
            pastValues[static_cast<size_t>(l)].size(), pastShape.data(),
            pastShape.size()));
        names2.emplace_back("past_key_values." + std::to_string(l) + ".value");
      }

      std::vector<const char*> inputNames2;
      inputNames2.reserve(names2.size());
      for (const auto& n : names2)
        inputNames2.push_back(n.c_str());

      auto outs = decoderMerged_->Run(
          Ort::RunOptions{}, inputNames2.data(), feeds2.data(), feeds2.size(),
          decoderOutputNames, kNumDecoderOutputs);
      for (int l = 0; l < kNumLayers; ++l) {
        copyPast(outs, 1 + 2 * l, l, true);
        copyPast(outs, 2 + 2 * l, l, false);
      }
      ++curLen;
      return outs;
    };

    // First generated token comes from the prefill logits (last position).
    {
      const int64_t best = pickBest(firstOuts);
      if (best == kEosId)
        return "";
      generated.push_back(best);
    }

    int64_t step = promptLen;
    for (int32_t i = 1; i < maxTokens; ++i) {
      auto outs = runDecoderStep(generated.back(), step);
      ++step;
      const int64_t best = pickBest(outs);
      if (best == kEosId)
        break;
      generated.push_back(best);
    }

    std::string caption;
    for (const auto& word : decodeTokens(generated))
      caption += word;
    return caption;
  }
  catch (const std::exception& e) {
    LOG_ERROR << "Vision: inference failed: " << e.what();
    return "";
  }
}

drogon::Task<std::string> VisionService::describeAsync(const VisionRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [req]() { return VisionService::describe(req); });
}
