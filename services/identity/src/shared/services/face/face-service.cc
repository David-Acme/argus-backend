#include "face-service.hxx"

#include <algorithm>
#include <array>
#include <cmath>
#include <drogon/drogon.h>
#include <gpu.h>
#include <memory>
#include <mutex>
#include <net.h>
#include <opencv2/imgproc.hpp>
#include <pipelinecache.h>
#include <runtime/blocking-task.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/thread-budget.hxx>
#include <shared/services/face/face-db.hxx>
#include <sqlite/vec-db.hxx>
#include <vector>

namespace
{
constexpr int kVisitorCropMaxSide = 192;
constexpr float kVisitorCropMargin = 0.35F;
constexpr int kVisitorCropQuality = 88;

void resumeOnLoop(std::coroutine_handle<> handle)
{
  blocking_task::resumeOn(blocking_task::resumeLoopFor(nullptr), handle);
}
}

FaceService::FaceService() : slots_(resumeOnLoop), faceDb_(VecDb::instance()) {}

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
  const int threads = ThreadBudget::computeThreads();

  auto* vkdev = HardwareProbe::get().vulkanDiscrete ? ncnn::get_gpu_device(0)
                                                    : nullptr;
  if (vkdev) {
    pipelineCache = std::make_unique<ncnn::PipelineCache>(vkdev);
    pipelineCache->load_cache((modelDir + "/face.ncnn.vkcache").c_str());
  }

  const auto configure = [&](ncnn::Net& net) {
    net.opt.use_packing_layout = true;
    net.opt.num_threads = threads;
    net.opt.use_vulkan_compute = vkdev != nullptr;
    net.opt.use_fp16_packed = true;
    net.opt.use_fp16_storage = true;
    net.opt.use_fp16_arithmetic = true;
    if (vkdev) {
      net.set_vulkan_device(vkdev);
      net.opt.pipeline_cache = pipelineCache.get();
    }
  };

  detector = std::make_unique<ncnn::Net>();
  configure(*detector);
  if (detector->load_param((modelDir + "/detector.param").c_str()) != 0 ||
      detector->load_model((modelDir + "/detector.bin").c_str()) != 0) {
    LOG_ERROR << "FaceService: failed to load detector model";
    return false;
  }

  recognizer = std::make_unique<ncnn::Net>();
  configure(*recognizer);
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
  auto impl = std::make_unique<Impl>();
  if (!impl->init(modelDir)) {
    LOG_WARN << "FaceService: models missing, recognition disabled";
    disable();
    return;
  }
  {
    const std::unique_lock lock(implMutex_);
    impl_ = std::move(impl);
  }
  faceDb_.init();
  slots_.open(static_cast<std::size_t>(ThreadBudget::inferenceSlots()));
  LOG_INFO << "FaceService initialized";
}

AntiSpoofLoad FaceService::initLiveness(const std::string& modelDir)
{
  const std::unique_lock lock(implMutex_);
  const AntiSpoofLoad load = antiSpoof_.load(modelDir);
  if (load != AntiSpoofLoad::Loaded)
    LOG_WARN << "Liveness: the anti-spoofing models are "
             << anti_spoof::loadToString(load) << " in " << modelDir
             << "; face login and registration stay refused while the check "
                "is required";
  return load;
}

void FaceService::disable()
{
  disabled_.store(true);
  slots_.open(static_cast<std::size_t>(ThreadBudget::inferenceSlots()));
}

void FaceService::shutdown()
{
  const std::unique_lock lock(implMutex_);
  faceDb_.shutdown();

  if (impl_ && impl_->pipelineCache)
    impl_->pipelineCache->save_cache("models/face/face.ncnn.vkcache");

  impl_.reset();
  antiSpoof_.unload();
  LOG_INFO << "FaceService shutdown";
}

bool FaceService::isLoaded() const
{
  const std::shared_lock lock(implMutex_);
  return impl_ != nullptr;
}

bool FaceService::livenessLoaded() const
{
  const std::shared_lock lock(implMutex_);
  return antiSpoof_.isLoaded();
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
  const Impl& impl = input.impl;
  const uint8_t* imageData = input.rgbData;
  const int width = input.width;
  const int height = input.height;

  constexpr int kTarget = 640;
  const float scale =
      static_cast<float>(kTarget) / static_cast<float>(std::max(width, height));
  const int resizedWidth =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * scale)), 1, kTarget);
  const int resizedHeight =
      std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * scale)), 1, kTarget);
  const float invScaleX = static_cast<float>(width) / static_cast<float>(resizedWidth);
  const float invScaleY = static_cast<float>(height) / static_cast<float>(resizedHeight);

  const ncnn::Mat resized =
      ncnn::Mat::from_pixels_resize(imageData, ncnn::Mat::PIXEL_RGB, width,
                                    height, resizedWidth, resizedHeight);
  ncnn::Mat detIn;
  ncnn::copy_make_border(resized, detIn, 0, kTarget - resizedHeight, 0,
                         kTarget - resizedWidth, ncnn::BORDER_CONSTANT, 0.0F);

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
          const float acx = 8.0F + static_cast<float>(c * stride);
          const float acy = 8.0F + static_cast<float>(r * stride);
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
            fb.lm[static_cast<std::size_t>(k * 2)] =
                (acx + (anchorSize + 1) * lm.channel(a * 10 + k * 2).row(r)[c]) *
                invScaleX;
            fb.lm[static_cast<std::size_t>(k * 2 + 1)] =
                (acy + (anchorSize + 1) * lm.channel(a * 10 + k * 2 + 1).row(r)[c]) *
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

  return nms(allBoxes, 0.4F);
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
                                            .landmarks = box.lm.data()});

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

  if (input.encodeFace)
    analysis.faceJpeg = face_image::encodeFaceCrop({.rgbData = input.rgbData,
                                                    .width = input.width,
                                                    .height = input.height,
                                                    .x1 = box.x1,
                                                    .y1 = box.y1,
                                                    .x2 = box.x2,
                                                    .y2 = box.y2,
                                                    .margin = kVisitorCropMargin,
                                                    .maxSide = kVisitorCropMaxSide,
                                                    .quality = kVisitorCropQuality});
  return analysis;
}

std::optional<FaceService::FaceAnalysis>
FaceService::analyzePixels(const ExtractInput& input, bool encodeFace)
{
  const std::shared_lock lock(implMutex_);
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

std::optional<FaceService::FaceAnalysis>
FaceService::analyzeBytes(const AnalyzeImageInput& input)
{
  const auto decoded = face_image::decodeToRgb(input.imageBytes);
  if (decoded.rgb.empty())
    return std::nullopt;
  return analyzePixels({.rgbData = decoded.rgb.data(),
                        .width = decoded.width,
                        .height = decoded.height},
                       input.encodeFace);
}

FaceService::FaceCheck FaceService::verifyBytes(const VerifyImageInput& input)
{
  FaceCheck check;
  const FaceCheckPolicy& policy = input.policy;
  const auto decoded = face_image::decodeToRgb(input.imageBytes);
  if (decoded.rgb.empty()) {
    check.status = FaceCheckStatus::Undecodable;
    return check;
  }

  const std::shared_lock lock(implMutex_);
  if (!impl_) {
    check.status = FaceCheckStatus::Unavailable;
    return check;
  }
  if (!face_check::livenessRunnable({.required = policy.livenessRequired,
                                     .engineLoaded = antiSpoof_.isLoaded()})) {
    check.status = FaceCheckStatus::LivenessUnavailable;
    return check;
  }

  const auto boxes = runDetector({.impl = *impl_,
                                  .rgbData = decoded.rgb.data(),
                                  .width = decoded.width,
                                  .height = decoded.height});
  const auto competing = std::ranges::count_if(boxes, [&policy](const FaceBox& box) {
    return box.score >= policy.quality.minDetectorScore;
  });
  if (boxes.empty()) {
    check.status = FaceCheckStatus::NoFace;
    return check;
  }
  const auto best = std::ranges::max_element(
      boxes, {}, [](const FaceBox& box) { return box.score; });
  auto analysis = embedBox({.impl = *impl_,
                            .rgbData = decoded.rgb.data(),
                            .width = decoded.width,
                            .height = decoded.height,
                            .box = *best,
                            .encodeFace = false});
  check.quality = analysis.quality;
  check.confidence = best->score;
  check.status = face_check::screen({.faces = static_cast<int>(std::max<std::ptrdiff_t>(competing, 1)),
                                     .quality = analysis.quality,
                                     .gate = policy.quality});
  if (check.status != FaceCheckStatus::Accepted)
    return check;

  if (policy.livenessRequired) {
    check.liveness = antiSpoof_.realScore({.rgbData = decoded.rgb.data(),
                                           .width = decoded.width,
                                           .height = decoded.height,
                                           .x1 = best->x1,
                                           .y1 = best->y1,
                                           .x2 = best->x2,
                                           .y2 = best->y2});
    if (!check.liveness) {
      check.status = FaceCheckStatus::LivenessUnavailable;
      return check;
    }
    check.status = face_check::livenessVerdict(policy, *check.liveness);
    if (check.status != FaceCheckStatus::Accepted)
      return check;
  }

  check.embedding = std::move(analysis.embedding);
  if (policy.encodePortrait)
    check.portraitJpeg = face_image::encodeFaceCrop({.rgbData = decoded.rgb.data(),
                                                     .width = decoded.width,
                                                     .height = decoded.height,
                                                     .x1 = best->x1,
                                                     .y1 = best->y1,
                                                     .x2 = best->x2,
                                                     .y2 = best->y2,
                                                     .margin = face_image::kPortraitMargin,
                                                     .maxSide = face_image::kPortraitMaxSide,
                                                     .quality = face_image::kPortraitQuality});
  return check;
}

std::optional<FaceService::FaceResult>
FaceService::extractImage(std::string imageBytes)
{
  if (disabled_.load())
    return std::nullopt;
  slots_.acquire();
  const InferenceSlots::Permit permit(slots_);
  auto analysis = analyzeBytes({.imageBytes = std::move(imageBytes), .encodeFace = false});
  if (!analysis)
    return std::nullopt;
  return FaceResult{.embedding = std::move(analysis->embedding),
                    .confidence = analysis->box.score};
}

drogon::Task<std::optional<FaceService::FaceResult>>
FaceService::extractImageAsync(std::string imageBytes)
{
  if (disabled_.load())
    co_return std::nullopt;
  co_await slots_.acquireAsync();
  const InferenceSlots::Permit permit(slots_);
  co_return co_await BlockingTask<std::optional<FaceService::FaceResult>>(
      [this, image = std::move(imageBytes)]() mutable
      -> std::optional<FaceService::FaceResult> {
        auto analysis = analyzeBytes({.imageBytes = std::move(image), .encodeFace = false});
        if (!analysis)
          return std::nullopt;
        return FaceResult{.embedding = std::move(analysis->embedding),
                          .confidence = analysis->box.score};
      },
      BlockingLane::Heavy);
}

std::optional<FaceService::FaceAnalysis>
FaceService::analyzeImage(const AnalyzeImageInput& input)
{
  if (disabled_.load())
    return std::nullopt;
  slots_.acquire();
  const InferenceSlots::Permit permit(slots_);
  return analyzeBytes(input);
}

drogon::Task<std::optional<FaceService::FaceAnalysis>>
FaceService::analyzeImageAsync(AnalyzeImageInput input)
{
  if (disabled_.load())
    co_return std::nullopt;
  co_await slots_.acquireAsync();
  const InferenceSlots::Permit permit(slots_);
  co_return co_await BlockingTask<std::optional<FaceService::FaceAnalysis>>(
      [this, request = std::move(input)]() { return analyzeBytes(request); },
      BlockingLane::Heavy);
}

FaceService::FaceCheck FaceService::verifyImage(const VerifyImageInput& input)
{
  if (disabled_.load())
    return FaceCheck{};
  slots_.acquire();
  const InferenceSlots::Permit permit(slots_);
  return verifyBytes(input);
}

drogon::Task<FaceService::FaceCheck>
FaceService::verifyImageAsync(VerifyImageInput input)
{
  if (disabled_.load())
    co_return FaceCheck{};
  co_await slots_.acquireAsync();
  const InferenceSlots::Permit permit(slots_);
  co_return co_await BlockingTask<FaceCheck>(
      [this, request = std::move(input)]() { return verifyBytes(request); },
      BlockingLane::Heavy);
}
