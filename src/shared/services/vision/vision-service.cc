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

constexpr int kDefaultImageSize = 768;
constexpr int64_t kEosId = 2;
constexpr int64_t kPadId = 1;
constexpr int64_t kUnkId = 3;
constexpr int kNumLayers = 6;
constexpr int kNumHeads = 12;
constexpr int kHeadDim = 64;
constexpr int kEmbedDim = 768;
constexpr int kNoRepeatNgram = 3;
constexpr int kNumOutputs = 25;

const std::string kModelDir = "models/vision/florence";
const std::string kVisionEncoderPath = kModelDir + "/vision_encoder_int8.onnx";
const std::string kEncoderPath = kModelDir + "/encoder_model_int8.onnx";
const std::string kDecoderMergedPath =
    kModelDir + "/decoder_model_merged_int8.onnx";
const std::string kEmbedTokensPath = kModelDir + "/embed_tokens_int8.onnx";
const std::string kTokenizerPath = kModelDir + "/tokenizer.json";

const float kMean[3] = {0.485F, 0.456F, 0.406F};
const float kStd[3] = {0.229F, 0.224F, 0.225F};

const char* kDecoderOutputNames[kNumOutputs] = {
    "logits",
    "present.0.decoder.key",   "present.0.decoder.value",
    "present.0.encoder.key",   "present.0.encoder.value",
    "present.1.decoder.key",   "present.1.decoder.value",
    "present.1.encoder.key",   "present.1.encoder.value",
    "present.2.decoder.key",   "present.2.decoder.value",
    "present.2.encoder.key",   "present.2.encoder.value",
    "present.3.decoder.key",   "present.3.decoder.value",
    "present.3.encoder.key",   "present.3.encoder.value",
    "present.4.decoder.key",   "present.4.decoder.value",
    "present.4.encoder.key",   "present.4.encoder.value",
    "present.5.decoder.key",   "present.5.decoder.value",
    "present.5.encoder.key",   "present.5.encoder.value",
};

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
std::unique_ptr<Ort::Session> VisionService::encoder_;
std::unique_ptr<Ort::Session> VisionService::decoderMerged_;
std::unique_ptr<Ort::Session> VisionService::embedTokens_;
Ort::Env VisionService::env_{ORT_LOGGING_LEVEL_ERROR, "Argus-Vision"};
std::mutex VisionService::mutex_;
bool VisionService::loaded_ = false;
int VisionService::imageSize_ = kDefaultImageSize;
VisionService::FrameCache VisionService::cacheA_;
VisionService::FrameCache VisionService::cacheB_;
int VisionService::lastCacheSlot_ = 0;
std::vector<std::string> VisionService::idToToken_;
std::unordered_set<int64_t> VisionService::specialIds_;

const std::vector<int64_t>& VisionService::taskIds()
{
  // "<MORE_DETAILED_CAPTION>" -> "Describe with a paragraph what is shown in
  // the image." (byte-level BPE ids, <s> prefix + </s> suffix).
  static const std::vector<int64_t> ids = {0, 47066, 21700, 19, 10, 17818,
                                           99, 16,    2343,  11, 5,  2274,
                                           4,  2};
  return ids;
}

void VisionService::init()
{
  try {
    auto opts = Ort::SessionOptions{};
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(ThreadBudget::computeThreads());
    opts.SetInterOpNumThreads(1);

    // The vision encoder is the heaviest pass (DaViT over 24x24 patches);
    // give it the full batch thread budget.
    auto visionOpts = Ort::SessionOptions{};
    visionOpts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    visionOpts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    visionOpts.SetIntraOpNumThreads(ThreadBudget::computeThreads());
    visionOpts.SetInterOpNumThreads(1);

    visionEncoder_ =
        std::make_unique<Ort::Session>(env_, kVisionEncoderPath.c_str(), visionOpts);
    encoder_ = std::make_unique<Ort::Session>(env_, kEncoderPath.c_str(), opts);
    decoderMerged_ = std::make_unique<Ort::Session>(
        env_, kDecoderMergedPath.c_str(), opts);
    embedTokens_ =
        std::make_unique<Ort::Session>(env_, kEmbedTokensPath.c_str(), opts);

    std::ifstream tokFile(kTokenizerPath);
    if (!tokFile.is_open())
      throw std::runtime_error("failed to open tokenizer: " + kTokenizerPath);
    json tok;
    tokFile >> tok;

    idToToken_.assign(51289, "");
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

    imageSize_ = ConfigService::getInt("vision.image_size");
    if (imageSize_ < 384 || imageSize_ > 1024)
      imageSize_ = kDefaultImageSize;

    loaded_ = true;
    LOG_INFO << "Vision loaded: Florence-2-base-ft (ONNX int8, threads="
             << ThreadBudget::computeThreads()
             << ", image_size=" << imageSize_ << ")";
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
  encoder_.reset();
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
  cv::resize(src, resized, cv::Size(imageSize_, imageSize_), 0, 0,
             cv::INTER_CUBIC);

  std::vector<float> pixels(static_cast<size_t>(3) * imageSize_ * imageSize_);
  const float scale = 1.0F / 255.0F;
  size_t idx = 0;
  for (int c = 0; c < 3; ++c) {
    const float mean = kMean[c];
    const float invStd = 1.0F / kStd[c];
    for (int y = 0; y < imageSize_; ++y) {
      const uint8_t* row = resized.ptr<uint8_t>(y);
      for (int x = 0; x < imageSize_; ++x)
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
    if (id < kPadId || id == kEosId || id == kUnkId)
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
    const int64_t numImageTokens =
        static_cast<int64_t>(imageSize_ / 32) *
            static_cast<int64_t>(imageSize_ / 32) + 1;
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
      const char* visionIn[] = {"pixel_values"};
      const char* visionOut[] = {"image_features"};
      const std::vector<int64_t> pixelShape = {1, 3, imageSize_,
                                               imageSize_};
      auto pixelValue = Ort::Value::CreateTensor<float>(
          cpuMem(), pixels.data(), pixels.size(), pixelShape.data(),
          pixelShape.size());
      auto visionOuts = visionEncoder_->Run(Ort::RunOptions{}, visionIn,
                                            &pixelValue, 1, visionOut, 1);
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

    const auto& taskIds = VisionService::taskIds();
    std::vector<int64_t> taskIdVec(taskIds.begin(), taskIds.end());
    const std::vector<int64_t> taskShape = {
        1, static_cast<int64_t>(taskIdVec.size())};
    auto taskInput = Ort::Value::CreateTensor<int64_t>(
        cpuMem(), taskIdVec.data(), taskIdVec.size(), taskShape.data(),
        taskShape.size());
    const char* embedIn[] = {"input_ids"};
    const char* embedOut[] = {"inputs_embeds"};
    auto textOuts = embedTokens_->Run(Ort::RunOptions{}, embedIn,
                                      &taskInput, 1, embedOut, 1);

    const int64_t textLen = static_cast<int64_t>(taskIdVec.size());
    std::vector<float> textEmbeds(
        textOuts[0].GetTensorData<float>(),
        textOuts[0].GetTensorData<float>() +
            static_cast<size_t>(textLen) * kEmbedDim);

    std::vector<float> encEmbeds(
        static_cast<size_t>(numImageTokens + textLen) * kEmbedDim);
    std::memcpy(encEmbeds.data(), imageFeatures.data(),
                static_cast<size_t>(numImageTokens) * kEmbedDim *
                    sizeof(float));
    std::memcpy(encEmbeds.data() + numImageTokens * kEmbedDim,
                textEmbeds.data(),
                static_cast<size_t>(textLen) * kEmbedDim * sizeof(float));

    const int64_t encLen = numImageTokens + textLen;
    std::vector<int64_t> encAttnData(static_cast<size_t>(encLen), 1);
    const std::vector<int64_t> attnShape = {1, encLen};
    auto encAttn = Ort::Value::CreateTensor<int64_t>(
        cpuMem(), encAttnData.data(), encAttnData.size(), attnShape.data(),
        attnShape.size());
    const std::vector<int64_t> encEmbShape = {1, encLen, kEmbedDim};
    auto encEmbedsValue = Ort::Value::CreateTensor<float>(
        cpuMem(), encEmbeds.data(), encEmbeds.size(), encEmbShape.data(),
        encEmbShape.size());

    const char* encIn[] = {"inputs_embeds", "attention_mask"};
    const char* encOut[] = {"last_hidden_state"};
    auto encOuts = encoder_->Run(Ort::RunOptions{}, encIn,
                                 &encEmbedsValue, 2, encOut, 1);

    std::vector<float> encHidden(
        encOuts[0].GetTensorData<float>(),
        encOuts[0].GetTensorData<float>() +
            static_cast<size_t>(encLen) * kEmbedDim);

    std::vector<int64_t> generated;
    std::vector<std::vector<float>> pastDecK(kNumLayers), pastDecV(kNumLayers);
    std::vector<std::vector<float>> pastEncK(kNumLayers), pastEncV(kNumLayers);
    int64_t pastLen = 0;

    auto banNgrams = [&](int64_t candidate) {
      if (generated.size() < static_cast<size_t>(kNoRepeatNgram - 1))
        return false;
      const int64_t g1 = generated[generated.size() - 2];
      const int64_t g2 = generated.back();
      for (size_t i = 0; i + kNoRepeatNgram <= generated.size(); ++i) {
        if (generated[i] == g1 && generated[i + 1] == g2 &&
            generated[i + 2] == candidate)
          return true;
      }
      return false;
    };

    auto runDecoder = [&](int64_t token,
                          bool useCache) -> std::vector<Ort::Value> {
      auto decEmbeds = embed(token);
      std::vector<Ort::Value> feeds;
      std::vector<std::string> names;

      const std::vector<int64_t> decEmbShape = {1, 1, kEmbedDim};
      feeds.push_back(Ort::Value::CreateTensor<float>(
          cpuMem(), decEmbeds.data(), decEmbeds.size(), decEmbShape.data(),
          decEmbShape.size()));
      names.emplace_back("inputs_embeds");

      feeds.push_back(Ort::Value::CreateTensor<int64_t>(
          cpuMem(), encAttnData.data(), encAttnData.size(), attnShape.data(),
          attnShape.size()));
      names.emplace_back("encoder_attention_mask");

      const std::vector<int64_t> encHidShape = {1, encLen, kEmbedDim};
      feeds.push_back(Ort::Value::CreateTensor<float>(
          cpuMem(), encHidden.data(), encHidden.size(), encHidShape.data(),
          encHidShape.size()));
      names.emplace_back("encoder_hidden_states");

      for (int l = 0; l < kNumLayers; ++l) {
        // All 24 past inputs are required on every step; the first step
        // passes empty caches (the If branch ignores them).
        static const std::vector<float> kEmptyPast(1, 0.0F);
        const auto* decK =
            useCache ? &pastDecK[static_cast<size_t>(l)] : &kEmptyPast;
        const auto* decV =
            useCache ? &pastDecV[static_cast<size_t>(l)] : &kEmptyPast;
        const auto* encK =
            useCache ? &pastEncK[static_cast<size_t>(l)] : &kEmptyPast;
        const auto* encV =
            useCache ? &pastEncV[static_cast<size_t>(l)] : &kEmptyPast;
        const int64_t decLen = useCache ? pastLen : 0;
        const int64_t encLenPast = useCache ? encLen : 0;
        const std::vector<int64_t> decPastShape = {1, kNumHeads, decLen,
                                                   kHeadDim};
        const std::vector<int64_t> encPastShape = {1, kNumHeads, encLenPast,
                                                   kHeadDim};
        feeds.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), const_cast<float*>(decK->data()), decK->size(),
            decPastShape.data(), decPastShape.size()));
        names.push_back("past_key_values." + std::to_string(l) +
                        ".decoder.key");
        feeds.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), const_cast<float*>(decV->data()), decV->size(),
            decPastShape.data(), decPastShape.size()));
        names.push_back("past_key_values." + std::to_string(l) +
                        ".decoder.value");
        feeds.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), const_cast<float*>(encK->data()), encK->size(),
            encPastShape.data(), encPastShape.size()));
        names.push_back("past_key_values." + std::to_string(l) +
                        ".encoder.key");
        feeds.push_back(Ort::Value::CreateTensor<float>(
            cpuMem(), const_cast<float*>(encV->data()), encV->size(),
            encPastShape.data(), encPastShape.size()));
        names.push_back("past_key_values." + std::to_string(l) +
                        ".encoder.value");
      }

      bool useCacheFlag = useCache;
      const std::vector<int64_t> cacheShape = {1};
      feeds.push_back(Ort::Value::CreateTensor<bool>(
          cpuMem(), &useCacheFlag, 1, cacheShape.data(), cacheShape.size()));
      names.emplace_back("use_cache_branch");

      std::vector<const char*> inputNames;
      inputNames.reserve(names.size());
      for (const auto& n : names)
        inputNames.push_back(n.c_str());

      return decoderMerged_->Run(Ort::RunOptions{}, inputNames.data(),
                                 feeds.data(), feeds.size(),
                                 kDecoderOutputNames, kNumOutputs);
    };

    auto copyPast = [](const std::vector<Ort::Value>& outs, int outIdx,
                       std::vector<float>& dst) {
      const auto sh =
          outs[static_cast<size_t>(outIdx)].GetTensorTypeAndShapeInfo()
              .GetShape();
      dst.assign(outs[static_cast<size_t>(outIdx)].GetTensorData<float>(),
                 outs[static_cast<size_t>(outIdx)].GetTensorData<float>() +
                     static_cast<size_t>(sh[0] * sh[1] * sh[2] * sh[3]));
    };

    auto pickBest = [&](const std::vector<Ort::Value>& outs) {
      const auto logitsShape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
      const int64_t vocab = logitsShape[2];
      const auto* logitsBegin = outs[0].GetTensorData<float>();
      std::vector<float> logitsRow(logitsBegin,
                                   logitsBegin + static_cast<size_t>(vocab));
      int64_t best = 0;
      float bestScore = -1e30F;
      for (int64_t v = 0; v < vocab; ++v) {
        if (banNgrams(v))
          continue;
        const float s = logitsRow[static_cast<size_t>(v)];
        if (s > bestScore) {
          bestScore = s;
          best = v;
        }
      }
      return best;
    };

    // First decoder step: no cache, encoder past is produced here.
    {
      auto outs = runDecoder(kEosId, false);
      const int64_t best = pickBest(outs);
      for (int l = 0; l < kNumLayers; ++l) {
        copyPast(outs, 1 + l * 4, pastDecK[static_cast<size_t>(l)]);
        copyPast(outs, 2 + l * 4, pastDecV[static_cast<size_t>(l)]);
        copyPast(outs, 3 + l * 4, pastEncK[static_cast<size_t>(l)]);
        copyPast(outs, 4 + l * 4, pastEncV[static_cast<size_t>(l)]);
      }
      pastLen = 1;
      if (best == kEosId)
        return "";
      generated.push_back(best);
    }

    for (int32_t step = 1; step < req.maxTokens; ++step) {
      auto outs = runDecoder(generated.back(), true);
      const int64_t best = pickBest(outs);
      for (int l = 0; l < kNumLayers; ++l) {
        copyPast(outs, 1 + l * 4, pastDecK[static_cast<size_t>(l)]);
        copyPast(outs, 2 + l * 4, pastDecV[static_cast<size_t>(l)]);
      }
      ++pastLen;
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
