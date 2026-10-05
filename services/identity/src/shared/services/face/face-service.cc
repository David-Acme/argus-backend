#include "face-service.hxx"

#include <algorithm>
#include <allocator.h>
#include <array>
#include <cmath>
#include <cstring>
#include <drogon/drogon.h>
#include <gpu.h>
#include <memory>
#include <net.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <pipelinecache.h>
#include <shared/services/face/face-db.hxx>
#include <sqlite/vec-db.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/thread-budget.hxx>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb_image.h>
#pragma GCC diagnostic pop
#include <thread>
#include <vector>

FaceService::FaceService() : faceDb_(VecDb::instance()) {}

FaceService::~FaceService()
{
  shutdown();
}

FaceService& FaceService::instance()
{
  static FaceService service;
  return service;
}

bool FaceService::Impl::init(const std::string& modelDir)
{
  int threads = ThreadBudget::computeThreads();

  auto* vkdev = HardwareProbe::get().vulkanDiscrete ? ncnn::get_gpu_device(0)
                                                    : nullptr;
  if (vkdev) {
    blobAllocator = std::make_unique<ncnn::VkBlobAllocator>(vkdev);
    stagingAllocator = std::make_unique<ncnn::VkStagingAllocator>(vkdev);
    pipelineCache = std::make_unique<ncnn::PipelineCache>(vkdev);
    pipelineCache->load_cache((modelDir + "/face.ncnn.vkcache").c_str());
  }

  detector = std::make_unique<ncnn::Net>();
  detector->opt.use_packing_layout = true;
  detector->opt.num_threads = threads;
  detector->opt.use_vulkan_compute = vkdev != nullptr;
  detector->opt.use_fp16_packed = true;
  detector->opt.use_fp16_storage = true;
  detector->opt.use_fp16_arithmetic = true;

  if (vkdev) {
    detector->set_vulkan_device(vkdev);
    detector->opt.blob_vkallocator = blobAllocator.get();
    detector->opt.workspace_vkallocator = blobAllocator.get();
    detector->opt.staging_vkallocator = stagingAllocator.get();
    detector->opt.pipeline_cache = pipelineCache.get();
  }

  if (detector->load_param((modelDir + "/detector.param").c_str()) != 0 ||
      detector->load_model((modelDir + "/detector.bin").c_str()) != 0) {
    LOG_ERROR << "FaceService: failed to load detector model";
    return false;
  }

  recognizer = std::make_unique<ncnn::Net>();
  recognizer->opt.use_packing_layout = true;
  recognizer->opt.num_threads = threads;
  recognizer->opt.use_vulkan_compute = vkdev != nullptr;
  recognizer->opt.use_fp16_packed = true;
  recognizer->opt.use_fp16_storage = true;
  recognizer->opt.use_fp16_arithmetic = true;

  if (vkdev) {
    recognizer->set_vulkan_device(vkdev);
    recognizer->opt.blob_vkallocator = blobAllocator.get();
    recognizer->opt.workspace_vkallocator = blobAllocator.get();
    recognizer->opt.staging_vkallocator = stagingAllocator.get();
    recognizer->opt.pipeline_cache = pipelineCache.get();
  }

  if (recognizer->load_param((modelDir + "/recognizer.param").c_str()) != 0 ||
      recognizer->load_model((modelDir + "/recognizer.bin").c_str()) != 0) {
    LOG_ERROR << "FaceService: failed to load recognizer model";
    return false;
  }

  LOG_INFO << "FaceService: models loaded (threads=" << threads << ")"
           << " detector=" << (vkdev ? "vulkan" : "cpu")
           << " recognizer=" << (vkdev ? "vulkan" : "cpu")
           << " fp16=" << (detector->opt.use_fp16_storage ? "on" : "off");
  return true;
}

void FaceService::init()
{
  init("models/face");
}

void FaceService::init(const std::string& modelDir)
{
  impl_ = std::make_unique<Impl>();

  if (!impl_->init(modelDir)) {
    impl_.reset();
    LOG_WARN << "FaceService: models missing, recognition disabled";
    disable();
    return;
  }

  faceDb_.init();
  concurrency_.release(ThreadBudget::inferenceSlots());
  LOG_INFO << "FaceService initialized";
}

void FaceService::disable()
{
  disabled_.store(true);
  concurrency_.release(ThreadBudget::inferenceSlots());
}

void FaceService::shutdown()
{
  std::scoped_lock lock(implMutex_);
  faceDb_.shutdown();

  if (impl_ && impl_->pipelineCache)
    impl_->pipelineCache->save_cache("models/face/face.ncnn.vkcache");

  impl_.reset();
  LOG_INFO << "FaceService shutdown";
}

bool FaceService::isLoaded() const
{
  std::scoped_lock lock(implMutex_);
  return impl_ != nullptr;
}

static std::vector<float> normalize(const float* v, int n)
{
  std::vector<float> out(v, v + n);
  float sq = 0.0F;
  for (auto x : out)
    sq += x * x;
  sq = std::sqrt(sq);
  if (sq > 1e-6F)
    for (auto& x : out)
      x /= sq;
  return out;
}

static float iou(const FaceService::FaceBox& a, const FaceService::FaceBox& b)
{
  float ix1 = std::max(a.x1, b.x1);
  float iy1 = std::max(a.y1, b.y1);
  float ix2 = std::min(a.x2, b.x2);
  float iy2 = std::min(a.y2, b.y2);
  float iw = std::max(0.0F, ix2 - ix1 + 1);
  float ih = std::max(0.0F, iy2 - iy1 + 1);
  float ia = iw * ih;
  float ua = (a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1) +
             (b.x2 - b.x1 + 1) * (b.y2 - b.y1 + 1) - ia;
  return ia / (ua + 1e-6F);
}

static std::vector<FaceService::FaceBox>
nms(std::vector<FaceService::FaceBox> boxes, float thresh)
{
  std::ranges::sort(boxes, std::ranges::greater{},
                    &FaceService::FaceBox::score);
  std::vector<FaceService::FaceBox> out;
  std::vector<bool> suppressed(boxes.size(), false);
  for (size_t i = 0; i < boxes.size(); ++i) {
    if (suppressed[i])
      continue;
    out.push_back(boxes[i]);
    for (size_t j = i + 1; j < boxes.size(); ++j) {
      if (!suppressed[j] && iou(boxes[i], boxes[j]) > thresh)
        suppressed[j] = true;
    }
  }
  return out;
}

std::vector<FaceService::FaceBox>
FaceService::runDetector(const RunDetectorInput& input)
{
  Impl& impl = input.impl;
  const uint8_t* imageData = input.rgbData;
  const int width = input.width;
  const int height = input.height;

  constexpr int kTarget = 640;
  const float invScaleX = static_cast<float>(width) / kTarget;
  const float invScaleY = static_cast<float>(height) / kTarget;

  ncnn::Mat detIn =
      ncnn::Mat::from_pixels_resize(imageData, ncnn::Mat::PIXEL_RGB, width,
                                    height, kTarget, kTarget);

  ncnn::Extractor detEx = impl.detector->create_extractor();
  detEx.input("data", detIn);

  ncnn::Mat cls32, bbox32, lm32;
  ncnn::Mat cls16, bbox16, lm16;
  ncnn::Mat cls8, bbox8, lm8;

  detEx.extract("face_rpn_cls_prob_reshape_stride32", cls32);
  detEx.extract("face_rpn_bbox_pred_stride32", bbox32);
  detEx.extract("face_rpn_landmark_pred_stride32", lm32);
  detEx.extract("face_rpn_cls_prob_reshape_stride16", cls16);
  detEx.extract("face_rpn_bbox_pred_stride16", bbox16);
  detEx.extract("face_rpn_landmark_pred_stride16", lm16);
  detEx.extract("face_rpn_cls_prob_reshape_stride8", cls8);
  detEx.extract("face_rpn_bbox_pred_stride8", bbox8);
  detEx.extract("face_rpn_landmark_pred_stride8", lm8);

  std::vector<FaceBox> allBoxes;

  struct ScaleInput
  {
    const ncnn::Mat& cls;
    const ncnn::Mat& bbox;
    const ncnn::Mat& lm;
    int stride{0};
    float scale0{0.0F};
    float scale1{0.0F};
    float scoreThresh{0.0F};
  };

  auto processScale = [&](const ScaleInput& input) {
    const ncnn::Mat& cls = input.cls;
    const ncnn::Mat& bbox = input.bbox;
    const ncnn::Mat& lm = input.lm;
    const int stride = input.stride;
    const float scale0 = input.scale0;
    const float scale1 = input.scale1;
    const float scoreThresh = input.scoreThresh;
    for (int a = 0; a < 2; ++a) {
      const float anchorSize = 16.0F * (a == 0 ? scale0 : scale1);
      for (int r = 0; r < cls.h; ++r) {
        for (int c = 0; c < cls.w; ++c) {
          float score = cls.channel(2 + a)[r * cls.w + c];
          if (score < scoreThresh)
            continue;
          const float acx = 8.0F + c * stride;
          const float acy = 8.0F + r * stride;
          const float dx = bbox.channel(a * 4).row(r)[c];
          const float dy = bbox.channel(a * 4 + 1).row(r)[c];
          const float dw = bbox.channel(a * 4 + 2).row(r)[c];
          const float dh = bbox.channel(a * 4 + 3).row(r)[c];
          const float pbCx = acx + anchorSize * dx;
          const float pbCy = acy + anchorSize * dy;
          const float pbW = anchorSize * std::exp(dw);
          const float pbH = anchorSize * std::exp(dh);
          FaceBox fb;
          fb.x1 = (pbCx - pbW * 0.5F) * invScaleX;
          fb.y1 = (pbCy - pbH * 0.5F) * invScaleY;
          fb.x2 = (pbCx + pbW * 0.5F) * invScaleX;
          fb.y2 = (pbCy + pbH * 0.5F) * invScaleY;
          fb.score = score;
          for (int k = 0; k < 5; ++k) {
            fb.lm[k * 2] = (acx + (anchorSize + 1) *
                                      lm.channel(a * 10 + k * 2).row(r)[c]) *
                           invScaleX;
            fb.lm[k * 2 + 1] = (acy + (anchorSize + 1) *
                                            lm.channel(a * 10 + k * 2 + 1)
                                                .row(r)[c]) *
                               invScaleY;
          }
          allBoxes.push_back(fb);
        }
      }
    }
  };

  processScale({.cls = cls32,
                .bbox = bbox32,
                .lm = lm32,
                .stride = 32,
                .scale0 = 32.0F,
                .scale1 = 16.0F,
                .scoreThresh = 0.5F});
  processScale({.cls = cls16,
                .bbox = bbox16,
                .lm = lm16,
                .stride = 16,
                .scale0 = 8.0F,
                .scale1 = 4.0F,
                .scoreThresh = 0.5F});
  processScale({.cls = cls8,
                .bbox = bbox8,
                .lm = lm8,
                .stride = 8,
                .scale0 = 2.0F,
                .scale1 = 1.0F,
                .scoreThresh = 0.5F});

  auto kept = nms(allBoxes, 0.4F);
  return kept;
}

std::vector<FaceService::FaceBox>
FaceService::detectAll(const DetectAllInput& input)
{
  const uint8_t* imageData = input.rgbData;
  const int width = input.width;
  const int height = input.height;

  std::scoped_lock lock(implMutex_);
  if (!impl_)
    return {};
  return runDetector(
      {.impl = *impl_, .rgbData = imageData, .width = width, .height = height});
}

std::vector<uint8_t> FaceService::alignFace(const AlignFaceInput& input)
{
  constexpr std::array<float, 10> kReference = {
      30.2946F, 51.6963F, 65.5318F, 51.5014F, 48.0252F,
      71.7366F, 33.5493F, 92.3655F, 62.7299F, 92.2041F};
  std::array<float, 6> transform{};
  std::array<float, 6> inverse{};
  ncnn::get_affine_transform(input.landmarks, kReference.data(), 5,
                             transform.data());
  ncnn::invert_affine_transform(transform.data(), inverse.data());
  std::vector<uint8_t> aligned(static_cast<size_t>(kAlignedSide) *
                               kAlignedSide * 3);
  ncnn::warpaffine_bilinear_c3(input.rgbData, input.width, input.height,
                               aligned.data(), kAlignedSide, kAlignedSide,
                               inverse.data());
  return aligned;
}

FaceService::FaceAnalysis FaceService::embedBox(const EmbedBoxInput& input)
{
  constexpr int kAligned = kAlignedSide;
  const FaceBox& box = input.box;
  std::vector<uint8_t> aligned = alignFace({.rgbData = input.rgbData,
                                            .width = input.width,
                                            .height = input.height,
                                            .landmarks = box.lm});

  const ncnn::Mat in = ncnn::Mat::from_pixels(aligned.data(), ncnn::Mat::PIXEL_RGB,
                                              kAligned, kAligned);
  ncnn::Extractor extractor = input.impl.recognizer->create_extractor();
  extractor.input("data", in);
  ncnn::Mat embedding;
  extractor.extract("fc1", embedding);

  FaceAnalysis analysis;
  analysis.embedding =
      normalize(embedding.channel(0), embedding.w * embedding.h * embedding.c);
  analysis.box = box;
  analysis.quality = face_quality::geometry(
      {.leftEyeX = box.lm[0],
       .leftEyeY = box.lm[1],
       .rightEyeX = box.lm[2],
       .rightEyeY = box.lm[3],
       .noseX = box.lm[4],
       .noseY = box.lm[5],
       .mouthLeftX = box.lm[6],
       .mouthLeftY = box.lm[7],
       .mouthRightX = box.lm[8],
       .mouthRightY = box.lm[9]});
  analysis.quality.detectorScore = box.score;
  analysis.quality.faceWidthPx = box.x2 - box.x1;

  const cv::Mat alignedRgb(kAligned, kAligned, CV_8UC3, aligned.data());
  cv::Mat gray;
  cv::cvtColor(alignedRgb, gray, cv::COLOR_RGB2GRAY);
  cv::Mat laplacian;
  cv::Laplacian(gray, laplacian, CV_32F);
  cv::Scalar mean;
  cv::Scalar deviation;
  cv::meanStdDev(laplacian, mean, deviation);
  analysis.quality.sharpness =
      static_cast<float>(deviation[0] * deviation[0]);

  if (input.encodeFace) {
    const float faceWidth = box.x2 - box.x1;
    const float faceHeight = box.y2 - box.y1;
    const float margin = 0.35F * std::max(faceWidth, faceHeight);
    const int x1 = std::clamp(static_cast<int>(box.x1 - margin), 0, input.width - 1);
    const int y1 = std::clamp(static_cast<int>(box.y1 - margin), 0, input.height - 1);
    const int x2 = std::clamp(static_cast<int>(box.x2 + margin), x1 + 1, input.width);
    const int y2 = std::clamp(static_cast<int>(box.y2 + margin), y1 + 1, input.height);
    const cv::Mat full(input.height, input.width, CV_8UC3,
                       const_cast<uint8_t*>(input.rgbData));
    cv::Mat face;
    cv::cvtColor(full(cv::Rect(x1, y1, x2 - x1, y2 - y1)), face,
                 cv::COLOR_RGB2BGR);
    constexpr int kMaxCropSide = 192;
    const int side = std::max(face.cols, face.rows);
    if (side > kMaxCropSide) {
      const double scale = static_cast<double>(kMaxCropSide) / side;
      cv::resize(face, face, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    std::vector<uchar> buffer;
    if (cv::imencode(".jpg", face, buffer, {cv::IMWRITE_JPEG_QUALITY, 88}))
      analysis.faceJpeg.assign(buffer.begin(), buffer.end());
  }
  return analysis;
}

std::optional<FaceService::FaceAnalysis>
FaceService::analyzePixels(const ExtractInput& input, bool encodeFace)
{
  std::scoped_lock lock(implMutex_);
  if (!impl_)
    return std::nullopt;
  const auto boxes = runDetector({.impl = *impl_,
                                  .rgbData = input.rgbData,
                                  .width = input.width,
                                  .height = input.height});
  if (boxes.empty())
    return std::nullopt;
  const auto best = std::ranges::max_element(
      boxes, {}, [](const FaceBox& box) { return box.score; });
  auto analysis = embedBox({.impl = *impl_,
                            .rgbData = input.rgbData,
                            .width = input.width,
                            .height = input.height,
                            .box = *best,
                            .encodeFace = encodeFace});
  analysis.faces = static_cast<int>(boxes.size());
  return analysis;
}

std::optional<FaceService::FaceResult>
FaceService::extractFace(const ExtractFaceInput& input)
{
  std::scoped_lock lock(implMutex_);
  if (!impl_)
    return std::nullopt;
  auto analysis = embedBox({.impl = *impl_,
                            .rgbData = input.rgbData,
                            .width = input.width,
                            .height = input.height,
                            .box = input.box,
                            .encodeFace = false});
  return FaceResult{.embedding = std::move(analysis.embedding),
                    .confidence = input.box.score};
}

std::optional<FaceService::FaceResult>
FaceService::extract(const ExtractInput& input)
{
  auto analysis = analyzePixels(input, false);
  if (!analysis)
    return std::nullopt;
  return FaceResult{.embedding = std::move(analysis->embedding),
                    .confidence = analysis->box.score};
}

namespace
{

constexpr int kScaledDecodeThreshold = 2048;
constexpr int kScaledDecodeDeepThreshold = 4096;
constexpr int kMaxDecodeSide = 12000;
constexpr int64_t kMaxDecodePixels = int64_t{50} * 1000 * 1000;

struct DecodedImage
{
  std::vector<uint8_t> rgb;
  int width{0};
  int height{0};
};

DecodedImage decodeToRgb(const std::string& imageBytes)
{
  int origW = 0;
  int origH = 0;
  int channels = 0;
  stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(imageBytes.data()),
                        static_cast<int>(imageBytes.size()), &origW, &origH,
                        &channels);

  if (origW <= 0 || origH <= 0 || origW > kMaxDecodeSide ||
      origH > kMaxDecodeSide ||
      static_cast<int64_t>(origW) * origH > kMaxDecodePixels)
    return {};

  const int maxSide = std::max(origW, origH);

  if (maxSide > kScaledDecodeThreshold) {
    int flags = cv::IMREAD_COLOR;
    if (maxSide > kScaledDecodeDeepThreshold)
      flags = cv::IMREAD_REDUCED_COLOR_4;
    else
      flags = cv::IMREAD_REDUCED_COLOR_2;

    cv::Mat bgr =
        cv::imdecode(cv::Mat(1, static_cast<int>(imageBytes.size()), CV_8UC1,
                             const_cast<char*>(imageBytes.data())),
                     flags);
    if (bgr.empty())
      return {};

    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    return {.rgb = std::vector<uint8_t>(rgb.data, rgb.data + rgb.total() * 3),
            .width = rgb.cols,
            .height = rgb.rows};
  }

  int width = 0;
  int height = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>
      decoded(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(
                                        imageBytes.data()),
                                    static_cast<int>(imageBytes.size()), &width,
                                    &height, &channels, 3),
              &stbi_image_free);
  if (!decoded)
    return {};

  return {.rgb =
              std::vector<uint8_t>(decoded.get(),
                                   decoded.get() +
                                       static_cast<size_t>(width) * height * 3),
          .width = width,
          .height = height};
}

}

std::optional<int64_t> FaceService::identify(std::string imageBytes)
{
  if (disabled_.load())
    return std::nullopt;
  concurrency_.acquire();
  struct SlotGuard
  {
    ~SlotGuard() { owner->concurrency_.release(); }
    FaceService* owner;
  } slotGuard{this};

  const auto decoded = decodeToRgb(imageBytes);
  if (decoded.rgb.empty())
    return std::nullopt;

  auto faceResult = extract({.rgbData = decoded.rgb.data(),
                             .width = decoded.width,
                             .height = decoded.height});
  if (!faceResult)
    return std::nullopt;

  auto match = faceDb_.search(faceResult->embedding.data());
  if (!match)
    return std::nullopt;

  LOG_INFO << "FaceService::identify person=" << match->first
           << " confidence=" << match->second;

  return match->first;
}

drogon::Task<std::optional<int64_t>>
FaceService::identifyAsync(std::string imageBytes)
{
  co_return co_await BlockingTask<std::optional<int64_t>>(
      [this, image = std::move(imageBytes)]() mutable {
        return identify(std::move(image));
      },
      BlockingLane::Heavy);
}

std::optional<FaceService::FaceResult>
FaceService::extractImage(std::string imageBytes)
{
  if (disabled_.load())
    return std::nullopt;
  concurrency_.acquire();
  struct SlotGuard
  {
    ~SlotGuard() { owner->concurrency_.release(); }
    FaceService* owner;
  } slotGuard{this};

  const auto decoded = decodeToRgb(imageBytes);
  if (decoded.rgb.empty())
    return std::nullopt;

  return extract({.rgbData = decoded.rgb.data(),
                  .width = decoded.width,
                  .height = decoded.height});
}

drogon::Task<std::optional<FaceService::FaceResult>>
FaceService::extractImageAsync(std::string imageBytes)
{
  co_return co_await BlockingTask<std::optional<FaceService::FaceResult>>(
      [this, image = std::move(imageBytes)]() mutable {
        return extractImage(std::move(image));
      },
      BlockingLane::Heavy);
}

std::optional<FaceService::FaceAnalysis>
FaceService::analyzeImage(const AnalyzeImageInput& input)
{
  if (disabled_.load())
    return std::nullopt;
  concurrency_.acquire();
  struct SlotGuard
  {
    ~SlotGuard() { owner->concurrency_.release(); }
    FaceService* owner;
  } slotGuard{this};

  const auto decoded = decodeToRgb(input.imageBytes);
  if (decoded.rgb.empty())
    return std::nullopt;
  return analyzePixels({.rgbData = decoded.rgb.data(),
                        .width = decoded.width,
                        .height = decoded.height},
                       input.encodeFace);
}

drogon::Task<std::optional<FaceService::FaceAnalysis>>
FaceService::analyzeImageAsync(AnalyzeImageInput input)
{
  co_return co_await BlockingTask<std::optional<FaceService::FaceAnalysis>>(
      [this, request = std::move(input)]() { return analyzeImage(request); },
      BlockingLane::Heavy);
}
