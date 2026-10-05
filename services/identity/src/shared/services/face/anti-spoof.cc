#include "anti-spoof.hxx"

#include <algorithm>
#include <cmath>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>
#include <openssl/evp.h>
#include <runtime/thread-budget.hxx>

namespace
{
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

constexpr std::size_t kReadChunk = std::size_t{64} * 1024;
constexpr std::size_t kPlane =
    static_cast<std::size_t>(kAntiSpoofInputSide) * kAntiSpoofInputSide;

std::vector<float> tensorOf(const cv::Mat& bgr)
{
  std::vector<float> tensor(kPlane * 3);
  for (int y = 0; y < bgr.rows; ++y) {
    const auto* row = bgr.ptr<cv::Vec3b>(y);
    for (int x = 0; x < bgr.cols; ++x) {
      const auto offset = static_cast<std::size_t>(y) * kAntiSpoofInputSide +
                          static_cast<std::size_t>(x);
      for (int channel = 0; channel < 3; ++channel)
        tensor[static_cast<std::size_t>(channel) * kPlane + offset] =
            static_cast<float>(row[x][channel]);
    }
  }
  return tensor;
}
}

std::optional<AntiSpoofCrop> anti_spoof::cropBox(const AntiSpoofCropInput& input)
{
  if (input.imageWidth < 2 || input.imageHeight < 2 || input.width <= 0.0F ||
      input.height <= 0.0F || input.scale <= 0.0F)
    return std::nullopt;
  const auto srcW = static_cast<float>(input.imageWidth);
  const auto srcH = static_cast<float>(input.imageHeight);
  const float scale = std::min({(srcH - 1.0F) / input.height,
                                (srcW - 1.0F) / input.width, input.scale});
  const float newWidth = input.width * scale;
  const float newHeight = input.height * scale;
  const float centerX = input.x + input.width / 2.0F;
  const float centerY = input.y + input.height / 2.0F;

  float left = centerX - newWidth / 2.0F;
  float top = centerY - newHeight / 2.0F;
  float right = centerX + newWidth / 2.0F;
  float bottom = centerY + newHeight / 2.0F;
  if (left < 0.0F) {
    right -= left;
    left = 0.0F;
  }
  if (top < 0.0F) {
    bottom -= top;
    top = 0.0F;
  }
  if (right > srcW - 1.0F) {
    left -= right - srcW + 1.0F;
    right = srcW - 1.0F;
  }
  if (bottom > srcH - 1.0F) {
    top -= bottom - srcH + 1.0F;
    bottom = srcH - 1.0F;
  }
  AntiSpoofCrop crop{.x1 = std::max(0, static_cast<int>(left)),
                     .y1 = std::max(0, static_cast<int>(top)),
                     .x2 = static_cast<int>(right),
                     .y2 = static_cast<int>(bottom)};
  crop.x2 = std::clamp(crop.x2, crop.x1, input.imageWidth - 1);
  crop.y2 = std::clamp(crop.y2, crop.y1, input.imageHeight - 1);
  return crop;
}

std::array<float, kAntiSpoofClasses>
anti_spoof::softmax(std::span<const float, kAntiSpoofClasses> logits)
{
  const float top = *std::ranges::max_element(logits);
  std::array<float, kAntiSpoofClasses> out{};
  float sum = 0.0F;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = std::exp(logits[i] - top);
    sum += out[i];
  }
  for (auto& value : out)
    value /= sum;
  return out;
}

bool anti_spoof::isLive(float realScore, float threshold)
{
  return std::isfinite(realScore) && realScore >= threshold;
}

std::string_view anti_spoof::loadToString(AntiSpoofLoad load)
{
  switch (load) {
  case AntiSpoofLoad::Loaded:
    return "loaded";
  case AntiSpoofLoad::Missing:
    return "missing";
  case AntiSpoofLoad::ChecksumMismatch:
    return "checksum_mismatch";
  case AntiSpoofLoad::Invalid:
    return "invalid";
  }
  return "invalid";
}

std::optional<std::string> anti_spoof::sha256File(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    return std::nullopt;
  std::vector<char> chunk(kReadChunk);
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto read = file.gcount();
    if (read > 0 &&
        EVP_DigestUpdate(context.get(), chunk.data(), static_cast<std::size_t>(read)) != 1)
      return std::nullopt;
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    return std::nullopt;
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(static_cast<std::size_t>(length) * 2);
  for (unsigned int i = 0; i < length; ++i) {
    const auto byte = static_cast<unsigned>(digest[i]);
    hex.push_back(kHex[byte >> 4U]);
    hex.push_back(kHex[byte & 0x0FU]);
  }
  return hex;
}

AntiSpoofEngine::AntiSpoofEngine() = default;

AntiSpoofEngine::~AntiSpoofEngine() = default;

void AntiSpoofEngine::unload()
{
  sessions_.clear();
  env_.reset();
}

AntiSpoofLoad AntiSpoofEngine::load(const std::string& modelDir)
{
  unload();
  std::vector<std::pair<std::string, float>> verified;
  for (const auto& pin : kAntiSpoofModels) {
    const std::string path =
        (std::filesystem::path(modelDir) / std::string(pin.file)).string();
    const auto digest = anti_spoof::sha256File(path);
    if (!digest) {
      LOG_WARN << "Liveness: " << pin.file << " is missing from " << modelDir;
      return AntiSpoofLoad::Missing;
    }
    if (*digest != pin.sha256) {
      LOG_ERROR << "Liveness: " << pin.file
                << " does not match its pinned SHA-256; refusing to load it";
      return AntiSpoofLoad::ChecksumMismatch;
    }
    verified.emplace_back(path, pin.scale);
  }

  try {
    auto env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "argus-liveness");
    std::vector<Model> models;
    for (const auto& [path, scale] : verified) {
      Ort::SessionOptions options;
      options.SetIntraOpNumThreads(ThreadBudget::lightThreads());
      options.SetInterOpNumThreads(1);
      Model model;
      model.session = std::make_unique<Ort::Session>(*env, path.c_str(), options);
      model.scale = scale;
      Ort::AllocatorWithDefaultOptions allocator;
      model.inputName = model.session->GetInputNameAllocated(0, allocator).get();
      model.outputName = model.session->GetOutputNameAllocated(0, allocator).get();
      const auto shape =
          model.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      if (shape.size() != 4 || shape[1] != 3 || shape[2] != kAntiSpoofInputSide ||
          shape[3] != kAntiSpoofInputSide) {
        LOG_ERROR << "Liveness: " << path << " has an unexpected input shape";
        return AntiSpoofLoad::Invalid;
      }
      models.push_back(std::move(model));
    }
    env_ = std::move(env);
    sessions_ = std::move(models);
  }
  catch (const Ort::Exception& error) {
    LOG_ERROR << "Liveness: the anti-spoofing models could not be loaded: "
              << error.what();
    unload();
    return AntiSpoofLoad::Invalid;
  }
  LOG_INFO << "Liveness: MiniFASNet anti-spoofing loaded (" << sessions_.size()
           << " models)";
  return AntiSpoofLoad::Loaded;
}

std::optional<float> AntiSpoofEngine::realScore(const AntiSpoofScoreInput& input) const
{
  if (sessions_.empty() || input.rgbData == nullptr)
    return std::nullopt;
  const cv::Mat full(input.height, input.width, CV_8UC3,
                     const_cast<std::uint8_t*>(input.rgbData));
  const float boxWidth = input.x2 - input.x1 + 1.0F;
  const float boxHeight = input.y2 - input.y1 + 1.0F;
  const Ort::MemoryInfo memory =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const std::array<int64_t, 4> shape{1, 3, kAntiSpoofInputSide, kAntiSpoofInputSide};

  float realSum = 0.0F;
  for (const auto& model : sessions_) {
    const auto crop = anti_spoof::cropBox({.imageWidth = input.width,
                                           .imageHeight = input.height,
                                           .x = input.x1,
                                           .y = input.y1,
                                           .width = boxWidth,
                                           .height = boxHeight,
                                           .scale = model.scale});
    if (!crop)
      return std::nullopt;
    cv::Mat patch;
    cv::resize(full(cv::Rect(crop->x1, crop->y1, crop->width(), crop->height())),
               patch, cv::Size(kAntiSpoofInputSide, kAntiSpoofInputSide), 0.0, 0.0,
               cv::INTER_LINEAR);
    cv::cvtColor(patch, patch, cv::COLOR_RGB2BGR);
    auto tensor = tensorOf(patch);
    auto value = Ort::Value::CreateTensor<float>(memory, tensor.data(), tensor.size(),
                                                 shape.data(), shape.size());
    const std::array<const char*, 1> inputs{model.inputName.c_str()};
    const std::array<const char*, 1> outputs{model.outputName.c_str()};
    try {
      auto result = model.session->Run(Ort::RunOptions{nullptr}, inputs.data(), &value,
                                       1, outputs.data(), 1);
      const auto* logits = result.front().GetTensorData<float>();
      const std::span<const float, kAntiSpoofClasses> view(logits, kAntiSpoofClasses);
      realSum += anti_spoof::softmax(view)[kAntiSpoofRealClass];
    }
    catch (const Ort::Exception& error) {
      LOG_WARN << "Liveness: inference failed: " << error.what();
      return std::nullopt;
    }
  }
  return realSum / static_cast<float>(sessions_.size());
}
