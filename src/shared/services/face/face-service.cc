#include "face-service.hxx"

#include <algorithm>
#include <allocator.h>
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
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
// Vendored stb_image.h (ncnn's copy) defines unused API functions.
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
  impl_ = std::make_unique<Impl>();

  if (!impl_->init("models/face")) {
    impl_.reset();
    LOG_WARN << "FaceService: models missing, recognition disabled";
    return;
  }

  faceDb_.init();
  concurrency_.release(ThreadBudget::inferenceSlots());
  LOG_INFO << "FaceService initialized";
}

void FaceService::shutdown()
{
  std::lock_guard<std::mutex> lock(implMutex_);
  faceDb_.shutdown();

  if (impl_ && impl_->pipelineCache)
    impl_->pipelineCache->save_cache("models/face/face.ncnn.vkcache");

  impl_.reset();
  LOG_INFO << "FaceService shutdown";
}

bool FaceService::isLoaded() const
{
  std::lock_guard<std::mutex> lock(implMutex_);
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
  std::sort(boxes.begin(), boxes.end(),
            [](const FaceService::FaceBox& a, const FaceService::FaceBox& b) {
              return a.score > b.score;
            });
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
FaceService::runDetector(Impl& impl, const uint8_t* imageData, int width,
                         int height)
{
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

  auto processScale = [&](const ncnn::Mat& cls, const ncnn::Mat& bbox,
                          const ncnn::Mat& lm, int stride, float scale0,
                          float scale1, float scoreThresh) {
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

  processScale(cls32, bbox32, lm32, 32, 32.0F, 16.0F, 0.5F);
  processScale(cls16, bbox16, lm16, 16, 8.0F, 4.0F, 0.5F);
  processScale(cls8, bbox8, lm8, 8, 2.0F, 1.0F, 0.5F);

  auto kept = nms(allBoxes, 0.4F);
  return kept;
}

std::vector<FaceService::FaceBox>
FaceService::detectAll(const uint8_t* imageData, int width, int height)
{
  std::lock_guard<std::mutex> lock(implMutex_);
  if (!impl_)
    return {};
  return runDetector(*impl_, imageData, width, height);
}

std::optional<FaceService::FaceResult>
FaceService::extractFace(const uint8_t* imageData, int width, int height,
                         const FaceBox& fb)
{
  std::lock_guard<std::mutex> lock(implMutex_);
  if (!impl_)
    return std::nullopt;

  const float refPts[10] = {30.2946F, 51.6963F, 65.5318F, 51.5014F, 48.0252F,
                            71.7366F, 33.5493F, 92.3655F, 62.7299F, 92.2041F};

  float tm[6];
  float tmInv[6];
  ncnn::get_affine_transform(fb.lm, refPts, 5, tm);
  ncnn::invert_affine_transform(tm, tmInv);

  int roiX = std::clamp(static_cast<int>(fb.x1), 0, width - 1);
  int roiY = std::clamp(static_cast<int>(fb.y1), 0, height - 1);
  int roiW = std::clamp(static_cast<int>(fb.x2 - fb.x1 + 1), 1, width - roiX);
  int roiH = std::clamp(static_cast<int>(fb.y2 - fb.y1 + 1), 1, height - roiY);

  ncnn::Mat src =
      ncnn::Mat::from_pixels_roi(imageData, ncnn::Mat::PIXEL_RGB, width, height,
                                 roiX, roiY, roiW, roiH);

  ncnn::Mat warped;
  warped.create(112, 112, 3, static_cast<size_t>(1),
                static_cast<ncnn::Allocator*>(nullptr));
  ncnn::warpaffine_bilinear_c3(static_cast<const unsigned char*>(src.data),
                               src.w, src.h,
                               static_cast<unsigned char*>(warped.data), 112,
                               112, tmInv);

  ncnn::Mat aligned;
  aligned.create(112, 112, 3, static_cast<size_t>(4),
                 static_cast<ncnn::Allocator*>(nullptr));
  for (int c = 0; c < 3; ++c) {
    const unsigned char* s = warped.channel(c);
    float* d = aligned.channel(c);
    for (int i = 0; i < 112 * 112; ++i)
      d[i] = s[i];
  }

  ncnn::Extractor recEx = impl_->recognizer->create_extractor();
  recEx.input("data", aligned);

  ncnn::Mat emb;
  recEx.extract("fc1", emb);

  int dim = emb.w * emb.h * emb.c;
  auto result = FaceResult{normalize(emb.channel(0), dim), fb.score};

  return result;
}

std::optional<FaceService::FaceResult>
FaceService::extract(const uint8_t* imageData, int width, int height)
{
  std::vector<FaceBox> kept;
  {
    std::lock_guard<std::mutex> lock(implMutex_);
    if (!impl_)
      return std::nullopt;
    kept = runDetector(*impl_, imageData, width, height);
  }
  if (kept.empty())
    return std::nullopt;

  int bestIdx = 0;
  for (size_t i = 1; i < kept.size(); ++i)
    if (kept[i].score > kept[bestIdx].score)
      bestIdx = static_cast<int>(i);

  return extractFace(imageData, width, height, kept[bestIdx]);
}

namespace
{

constexpr int kScaledDecodeThreshold = 2048;
constexpr int kScaledDecodeDeepThreshold = 4096;

std::vector<uint8_t> decodeToRgb(const std::string& imageBytes, int& width,
                                 int& height)
{
  int origW = 0;
  int origH = 0;
  int channels = 0;
  stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(imageBytes.data()),
                        static_cast<int>(imageBytes.size()), &origW, &origH,
                        &channels);

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
    width = rgb.cols;
    height = rgb.rows;
    return std::vector<uint8_t>(rgb.data, rgb.data + rgb.total() * 3);
  }

  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>
      decoded(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(
                                        imageBytes.data()),
                                    static_cast<int>(imageBytes.size()), &width,
                                    &height, &channels, 3),
              &stbi_image_free);
  if (!decoded)
    return {};

  return std::vector<uint8_t>(decoded.get(),
                              decoded.get() +
                                  static_cast<size_t>(width) * height * 3);
}

} // namespace

std::optional<int64_t> FaceService::identify(std::string imageBytes)
{
  concurrency_.acquire();
  struct SlotGuard
  {
    ~SlotGuard() { owner->concurrency_.release(); }
    FaceService* owner;
  } slotGuard{this};

  int width = 0;
  int height = 0;
  auto rgb = decodeToRgb(imageBytes, width, height);
  if (rgb.empty())
    return std::nullopt;

  auto faceResult = extract(rgb.data(), width, height);
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
      });
}

std::optional<FaceService::FaceResult>
FaceService::extractImage(std::string imageBytes)
{
  concurrency_.acquire();
  struct SlotGuard
  {
    ~SlotGuard() { owner->concurrency_.release(); }
    FaceService* owner;
  } slotGuard{this};

  int width = 0;
  int height = 0;
  auto rgb = decodeToRgb(imageBytes, width, height);
  if (rgb.empty())
    return std::nullopt;

  return extract(rgb.data(), width, height);
}

drogon::Task<std::optional<FaceService::FaceResult>>
FaceService::extractImageAsync(std::string imageBytes)
{
  co_return co_await BlockingTask<std::optional<FaceService::FaceResult>>(
      [this, image = std::move(imageBytes)]() mutable {
        return extractImage(std::move(image));
      });
}
