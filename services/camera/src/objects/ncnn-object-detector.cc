#include <objects/ncnn-object-detector.hxx>

#include <allocator.h>
#include <gpu.h>
#include <net.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
constexpr float kPixelScale = 1.0f / 255.0f;
constexpr uint8_t kLetterboxGray = 114;

bool vulkanAvailable(const ObjectDetectorOptions& options)
{
  if (!options.useVulkan)
    return false;
  return HardwareProbe::get().vulkan;
}

struct RowScoreInput
{
  const float* row{nullptr};
  size_t rowLength{0};
  size_t classCount{0};
  int& cls;
};

float rowScore(const RowScoreInput& input)
{
  const float* row = input.row;
  const size_t rowLength = input.rowLength;
  const size_t classCount = input.classCount;
  int& cls = input.cls;
  if (rowLength == 6) {
    cls = static_cast<int>(row[5]);
    return row[4];
  }
  int best = 0;
  float bestScore = row[4];
  for (size_t c = 1; c < classCount; ++c) {
    if (row[4 + c] > bestScore) {
      bestScore = row[4 + c];
      best = static_cast<int>(c);
    }
  }
  cls = best;
  return bestScore;
}

std::string classNameFor(const std::vector<std::string>& classes, int cls)
{
  if (cls >= 0 && cls < static_cast<int>(classes.size()))
    return classes[cls];
  return "class_" + std::to_string(cls);
}
} // namespace

struct ObjectDetectorService::Impl
{
  std::string modelDir;
  std::unique_ptr<ncnn::Net> net;
  std::string inputBlob;
  std::string outputBlob;
  bool vulkan{false};
  bool shapeAccepted{false};
  size_t acceptedRowLength{0};
};

std::shared_ptr<ObjectDetectorService::Impl>
ObjectDetectorService::loadImpl(const std::string& modelDir, bool useVulkan)
{
  auto impl = std::make_shared<Impl>();
  impl->modelDir = modelDir;
  impl->vulkan = useVulkan;
  impl->net = std::make_unique<ncnn::Net>();
  impl->net->opt.use_packing_layout = true;
  impl->net->opt.num_threads = ThreadBudget::computeThreads();
  impl->net->opt.use_vulkan_compute = useVulkan;
  impl->net->opt.use_fp16_packed = true;
  impl->net->opt.use_fp16_storage = true;
  impl->net->opt.use_fp16_arithmetic = true;

  const std::string paramPath = modelDir + "/yolo26n.param";
  const std::string binPath = modelDir + "/yolo26n.bin";
  if (impl->net->load_param(paramPath.c_str()) != 0 ||
      impl->net->load_model(binPath.c_str()) != 0) {
    LOG_ERROR << "ObjectDetector: failed to load " << paramPath << " or "
              << binPath;
    return nullptr;
  }

  const auto& inputNames = impl->net->input_names();
  const auto& outputNames = impl->net->output_names();
  if (inputNames.empty() || outputNames.empty()) {
    LOG_ERROR << "ObjectDetector: could not resolve input/output blob names"
                 " from " << paramPath;
    return nullptr;
  }
  impl->inputBlob = inputNames.front();
  impl->outputBlob = outputNames.front();
  return impl;
}

ObjectDetectorService::ObjectDetectorService(ObjectDetectorOptions options)
    : options_(std::move(options))
{
}

ObjectDetectorService::~ObjectDetectorService()
{
  std::lock_guard<std::mutex> lock(implMutex_);
  impl_.reset();
}

void ObjectDetectorService::init()
{
  auto impl = loadImpl(options_.modelDir, vulkanAvailable(options_));
  if (!impl) {
    LOG_WARN << "ObjectDetector: model unavailable at " << options_.modelDir
             << "; detection disabled";
    return;
  }

  LOG_INFO << "ObjectDetector: model loaded from " << options_.modelDir
           << " input=" << impl->inputBlob << " output=" << impl->outputBlob
           << " backend=" << (impl->vulkan ? "vulkan" : "cpu")
           << " input_size=" << options_.inputSize
           << " classes=" << options_.classes.size();

  {
    std::lock_guard<std::mutex> lock(implMutex_);
    impl_ = std::move(impl);
  }
  slots_.release(ThreadBudget::inferenceSlots());
}

bool ObjectDetectorService::isLoaded() const
{
  std::lock_guard<std::mutex> lock(implMutex_);
  return impl_ != nullptr;
}

std::string ObjectDetectorService::backend() const
{
  std::lock_guard<std::mutex> lock(implMutex_);
  if (!impl_)
    return "disabled";
  return impl_->vulkan ? "vulkan" : "cpu";
}

std::vector<DetectedObject> ObjectDetectorService::detect(const DetectInput& input)
{
  const uint8_t* rgb = input.rgb;
  const int width = input.width;
  const int height = input.height;
  if (!isLoaded() || rgb == nullptr || width <= 0 || height <= 0)
    return {};

  slots_.acquire();
  struct SlotRelease
  {
    std::counting_semaphore<16>& slots;
    ~SlotRelease() { slots.release(); }
  } guard{slots_};

  std::shared_ptr<Impl> impl;
  {
    std::lock_guard<std::mutex> lock(implMutex_);
    impl = impl_;
  }
  if (!impl)
    return {};

  auto result = runNet({.impl = *impl, .rgb = rgb, .width = width, .height = height});
  if (!result && impl->vulkan) {
    LOG_WARN << "ObjectDetector: vulkan inference failed; falling back to CPU"
                " on this instance";
    if (auto reloaded = loadImpl(impl->modelDir, false)) {
      {
        std::lock_guard<std::mutex> lock(implMutex_);
        impl_ = reloaded;
      }
      impl = std::move(reloaded);
      result = runNet({.impl = *impl, .rgb = rgb, .width = width, .height = height});
    }
  }
  if (!result)
    return {};
  return *result;
}

std::optional<std::vector<DetectedObject>>
ObjectDetectorService::runNet(const RunNetInput& input)
{
  Impl& impl = input.impl;
  const uint8_t* rgb = input.rgb;
  const int width = input.width;
  const int height = input.height;
  const int inputSize = options_.inputSize;
  const double scale =
      std::min(static_cast<double>(inputSize) / width,
               static_cast<double>(inputSize) / height);
  const int resizedW = std::max(1, static_cast<int>(std::lround(width * scale)));
  const int resizedH = std::max(1, static_cast<int>(std::lround(height * scale)));
  const int padX = (inputSize - resizedW) / 2;
  const int padY = (inputSize - resizedH) / 2;

  cv::Mat frame(height, width, CV_8UC3, const_cast<uint8_t*>(rgb));
  cv::Mat resized;
  cv::resize(frame, resized, cv::Size(resizedW, resizedH), 0, 0,
             cv::INTER_LINEAR);
  cv::Mat letterboxed;
  cv::copyMakeBorder(resized, letterboxed, padY, inputSize - resizedH - padY,
                     padX, inputSize - resizedW - padX, cv::BORDER_CONSTANT,
                     cv::Scalar(kLetterboxGray, kLetterboxGray, kLetterboxGray));

  ncnn::Mat in = ncnn::Mat::from_pixels(
      letterboxed.data, ncnn::Mat::PIXEL_RGB, inputSize, inputSize);
  const float normVals[3] = {kPixelScale, kPixelScale, kPixelScale};
  in.substract_mean_normalize(nullptr, normVals);

  ncnn::Extractor ex = impl.net->create_extractor();
  if (ex.input(impl.inputBlob.c_str(), in) != 0)
    return std::nullopt;
  ncnn::Mat out;
  if (ex.extract(impl.outputBlob.c_str(), out) != 0)
    return std::nullopt;

  const size_t rowLength = out.w;
  const size_t rowCount = rowLength == 0 ? 0 : out.total() / rowLength;
  const size_t e2eLength = 6;
  const size_t rawLength = 4 + options_.classes.size();
  if (rowLength != e2eLength && rowLength != rawLength) {
    if (!impl.shapeAccepted || impl.acceptedRowLength != rowLength) {
      LOG_ERROR << "ObjectDetector: unsupported output shape (rows="
                << rowCount << ", row_length=" << rowLength
                << "); expected " << e2eLength << " (e2e) or " << rawLength
                << " (raw one2one) — refusing to decode";
    }
    return std::nullopt;
  }
  if (!impl.shapeAccepted) {
    impl.shapeAccepted = true;
    impl.acceptedRowLength = rowLength;
    LOG_INFO << "ObjectDetector: output shape rows=" << rowCount
             << " row_length=" << rowLength << " ("
             << (rowLength == e2eLength ? "e2e topk" : "raw one2one + topk")
             << ")";
  }

  std::vector<float> rows;
  rows.reserve(out.total());
  if (out.dims == 2) {
    for (int y = 0; y < out.h; ++y)
      rows.insert(rows.end(), out.row(y), out.row(y) + out.w);
  } else if (out.dims == 3) {
    for (int c = 0; c < out.c; ++c) {
      const ncnn::Mat channel = out.channel(c);
      for (int y = 0; y < channel.h; ++y)
        rows.insert(rows.end(), channel.row(y), channel.row(y) + channel.w);
    }
  } else {
    if (!impl.shapeAccepted)
      LOG_ERROR << "ObjectDetector: unexpected output dims=" << out.dims;
    return std::nullopt;
  }

  return postProcess(
      {.rows = rows.data(),
       .rowCount = rowCount,
       .rowLength = rowLength,
       .plan = LetterboxPlan{static_cast<float>(scale), padX, padY},
       .width = width,
       .height = height});
}

std::vector<DetectedObject>
ObjectDetectorService::postProcess(const PostProcessInput& input) const
{
  const float* rows = input.rows;
  const size_t rowCount = input.rowCount;
  const size_t rowLength = input.rowLength;
  const LetterboxPlan& plan = input.plan;
  const int width = input.width;
  const int height = input.height;
  struct Candidate
  {
    DetectedObject object;
    float confidence;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(rowCount);
  for (size_t r = 0; r < rowCount; ++r) {
    const float* row = rows + r * rowLength;
    int cls = -1;
    const float score = rowScore({.row = row, .rowLength = rowLength,
                                  .classCount = options_.classes.size(),
                                  .cls = cls});
    if (score < options_.confidence)
      continue;

    const float x1 = std::clamp((row[0] - plan.padX) / plan.scale, 0.f,
                                static_cast<float>(width));
    const float y1 = std::clamp((row[1] - plan.padY) / plan.scale, 0.f,
                                static_cast<float>(height));
    const float x2 = std::clamp((row[2] - plan.padX) / plan.scale, 0.f,
                                static_cast<float>(width));
    const float y2 = std::clamp((row[3] - plan.padY) / plan.scale, 0.f,
                                static_cast<float>(height));
    const float boxW = x2 - x1;
    const float boxH = y2 - y1;
    if (boxW < 1 || boxH < 1)
      continue;

    DetectedObject object;
    object.name = classNameFor(options_.classes, cls);
    object.cls = cls;
    object.confidence = score;
    object.x = x1;
    object.y = y1;
    object.w = boxW;
    object.h = boxH;
    candidates.push_back({std::move(object), score});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.confidence > b.confidence;
            });
  if (candidates.size() > static_cast<size_t>(options_.maxDet))
    candidates.resize(options_.maxDet);

  std::vector<DetectedObject> objects;
  objects.reserve(candidates.size());
  for (auto& candidate : candidates)
    objects.push_back(std::move(candidate.object));
  return objects;
}
