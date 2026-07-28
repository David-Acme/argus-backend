#include "face-service.hxx"

#include <algorithm>
#include <allocator.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <drogon/drogon.h>
#include <gpu.h>
#include <net.h>
#include <opencv2/imgcodecs.hpp>
#include <pipelinecache.h>
#include <shared/services/face/face-db.hxx>
#include <thread>
#include <vector>

std::unique_ptr<FaceService::Impl> FaceService::impl_;

bool FaceService::Impl::init(const std::string& modelDir)
{
  int cpuCount = std::thread::hardware_concurrency();
  int threads = std::max(1, cpuCount / 2);

  auto* vkdev = ncnn::get_gpu_device(0);
  if (vkdev) {
    blobAllocator = std::make_unique<ncnn::VkBlobAllocator>(vkdev);
    stagingAllocator = std::make_unique<ncnn::VkStagingAllocator>(vkdev);
    pipelineCache = std::make_unique<ncnn::PipelineCache>(vkdev);
    pipelineCache->load_cache((modelDir + "/face.ncnn.vkcache").c_str());
  }

  detector = std::make_unique<ncnn::Net>();
  detector->opt.use_packing_layout = true;
  detector->opt.num_threads = threads;
  detector->opt.use_vulkan_compute = true;
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
  recognizer->opt.use_vulkan_compute = true;
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
           << " detector=vulkan recognizer=vulkan"
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

  FaceDB::init();
  LOG_INFO << "FaceService initialized";
}

void FaceService::shutdown()
{
  FaceDB::shutdown();

  if (impl_ && impl_->pipelineCache)
    impl_->pipelineCache->save_cache("models/face/face.ncnn.vkcache");

  impl_.reset();
  LOG_INFO << "FaceService shutdown";
}

bool FaceService::isLoaded()
{
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

struct FaceBox
{
  float x1, y1, x2, y2;
  float score;
  float lm[10];
};

static float iou(const FaceBox& a, const FaceBox& b)
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

static std::vector<FaceBox> nms(std::vector<FaceBox> boxes, float thresh)
{
  std::sort(boxes.begin(), boxes.end(), [](const FaceBox& a, const FaceBox& b) {
    return a.score > b.score;
  });
  std::vector<FaceBox> out;
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

std::optional<FaceService::FaceResult>
FaceService::extract(const uint8_t* imageData, int width, int height)
{
  if (!impl_)
    return std::nullopt;

  auto t0 = std::chrono::steady_clock::now();

  constexpr int kTarget = 640;
  const float invScaleX = static_cast<float>(width) / kTarget;
  const float invScaleY = static_cast<float>(height) / kTarget;

  ncnn::Mat detIn =
      ncnn::Mat::from_pixels_resize(imageData, ncnn::Mat::PIXEL_BGR2RGB, width,
                                    height, kTarget, kTarget);
  detIn.substract_mean_normalize(nullptr, nullptr);

  ncnn::Extractor detEx = impl_->detector->create_extractor();
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

  auto tDet = std::chrono::steady_clock::now();

  std::vector<FaceBox> allBoxes;

  auto processScale = [&](const ncnn::Mat& cls, const ncnn::Mat& bbox,
                          const ncnn::Mat& lm, int stride, float scoreThresh) {
    for (int r = 0; r < cls.h; ++r) {
      for (int c = 0; c < cls.w; ++c) {
        float score = cls.channel(0)[r * cls.w + c];
        if (score < scoreThresh)
          continue;
        float cx = (c + 0.5F) * stride;
        float cy = (r + 0.5F) * stride;
        const float* dp = bbox.channel(0);
        int off = (r * bbox.w + c) * 4;
        float x1 = cx + dp[off] * stride;
        float y1 = cy + dp[off + 1] * stride;
        float x2 = cx + dp[off + 2] * stride + 1;
        float y2 = cy + dp[off + 3] * stride + 1;
        FaceBox fb;
        fb.x1 = x1 * invScaleX;
        fb.y1 = y1 * invScaleY;
        fb.x2 = x2 * invScaleX;
        fb.y2 = y2 * invScaleY;
        fb.score = score;
        const float* lp = lm.channel(0);
        int lmOff = (r * lm.w + c) * 10;
        for (int k = 0; k < 5; ++k) {
          fb.lm[k * 2] = (cx + lp[lmOff + k * 2] * stride) * invScaleX;
          fb.lm[k * 2 + 1] = (cy + lp[lmOff + k * 2 + 1] * stride) * invScaleY;
        }
        allBoxes.push_back(fb);
      }
    }
  };

  processScale(cls32, bbox32, lm32, 32, 0.5F);
  processScale(cls16, bbox16, lm16, 16, 0.5F);
  processScale(cls8, bbox8, lm8, 8, 0.5F);

  auto kept = nms(allBoxes, 0.4F);
  if (kept.empty())
    return std::nullopt;

  int bestIdx = 0;
  for (size_t i = 1; i < kept.size(); ++i)
    if (kept[i].score > kept[bestIdx].score)
      bestIdx = static_cast<int>(i);

  const auto& fb = kept[bestIdx];

  // Affine transform via similarity
  const float refPts[10] = {30.2946F, 51.6963F, 65.5318F, 51.5014F, 48.0252F,
                            71.7366F, 33.5493F, 92.3655F, 62.7299F, 92.2041F};
  float sCx = 0, sCy = 0, rCx = 0, rCy = 0;
  for (int i = 0; i < 5; ++i) {
    sCx += fb.lm[i * 2];
    sCy += fb.lm[i * 2 + 1];
    rCx += refPts[i * 2];
    rCy += refPts[i * 2 + 1];
  }
  sCx /= 5;
  sCy /= 5;
  rCx /= 5;
  rCy /= 5;
  float sD = 0, rD = 0;
  for (int i = 0; i < 5; ++i) {
    float dx = fb.lm[i * 2] - sCx, dy = fb.lm[i * 2 + 1] - sCy;
    sD += std::sqrt(dx * dx + dy * dy);
    dx = refPts[i * 2] - rCx;
    dy = refPts[i * 2 + 1] - rCy;
    rD += std::sqrt(dx * dx + dy * dy);
  }
  float s = rD / (sD + 1e-6F);
  float M[6] = {s, 0, rCx - s * sCx, 0, s, rCy - s * sCy};

  int roiX = std::clamp(static_cast<int>(fb.x1), 0, width - 1);
  int roiY = std::clamp(static_cast<int>(fb.y1), 0, height - 1);
  int roiW = std::clamp(static_cast<int>(fb.x2 - fb.x1 + 1), 1, width - roiX);
  int roiH = std::clamp(static_cast<int>(fb.y2 - fb.y1 + 1), 1, height - roiY);

  ncnn::Mat src =
      ncnn::Mat::from_pixels_roi(imageData, ncnn::Mat::PIXEL_BGR2RGB, width,
                                 height, roiX, roiY, roiW, roiH);

  ncnn::Mat warped(112, 112, 3);
  for (int y = 0; y < 112; ++y) {
    for (int x = 0; x < 112; ++x) {
      float sx = (x - M[2]) / M[0], sy = (y - M[5]) / M[4];
      int ix = std::clamp(static_cast<int>(sx), 0, src.w - 1);
      int iy = std::clamp(static_cast<int>(sy), 0, src.h - 1);
      warped.channel(0)[y * 112 + x] = src.channel(0)[iy * src.w + ix];
      warped.channel(1)[y * 112 + x] = src.channel(1)[iy * src.w + ix];
      warped.channel(2)[y * 112 + x] = src.channel(2)[iy * src.w + ix];
    }
  }

  const float means[3] = {127.5F, 127.5F, 127.5F};
  const float norms[3] = {1.0F / 127.5F, 1.0F / 127.5F, 1.0F / 127.5F};
  warped.substract_mean_normalize(means, norms);

  ncnn::Extractor recEx = impl_->recognizer->create_extractor();
  recEx.input("data", warped);

  ncnn::Mat emb;
  recEx.extract("fc1", emb);

  auto tRec = std::chrono::steady_clock::now();

  int dim = emb.w * emb.h * emb.c;
  auto result = FaceResult{normalize(emb.channel(0), dim), fb.score};

  auto msDet =
      std::chrono::duration_cast<std::chrono::milliseconds>(tDet - t0).count();
  auto msRec =
      std::chrono::duration_cast<std::chrono::milliseconds>(tRec - tDet)
          .count();
  auto msTotal =
      std::chrono::duration_cast<std::chrono::milliseconds>(tRec - t0).count();
  LOG_INFO << "FaceService: detect=" << msDet << "ms rec=" << msRec
           << "ms total=" << msTotal << "ms";

  return result;
}

std::optional<int64_t> FaceService::identify(const std::string& imageBytes)
{
  std::vector<uint8_t> buf(imageBytes.begin(), imageBytes.end());
  cv::Mat raw = cv::imdecode(buf, cv::IMREAD_COLOR);
  if (raw.empty())
    return std::nullopt;

  auto faceResult = extract(raw.data, raw.cols, raw.rows);
  if (!faceResult)
    return std::nullopt;

  auto match = FaceDB::search(faceResult->embedding.data());
  if (!match)
    return std::nullopt;

  LOG_INFO << "FaceService::identify person=" << match->first
           << " confidence=" << match->second;

  return match->first;
}
