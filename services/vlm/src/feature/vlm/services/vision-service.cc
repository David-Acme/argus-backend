#include "vision-service.hxx"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <drogon/drogon.h>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>
#include <opencv2/imgproc.hpp>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <feature/vlm/services/vision-hash.hxx>
#include <runtime/ai-init.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/thread-budget.hxx>
#include <vlm/vlm-errors.hxx>

namespace
{

constexpr const char* kDefaultModel = "models/vision/lfm2vl-25/lm-Q8_0.gguf";
constexpr const char* kDefaultMmproj =
    "models/vision/lfm2vl-25/mmproj-F16.gguf";
constexpr const char* kFallbackPrompt = "Can you describe this image?";

struct CaptionKeyInput
{
  const cv::Mat& image;
  const std::string& prompt;
  int32_t maxTokens{0};
};

uint64_t captionKey(const CaptionKeyInput& input)
{
  const std::string asked = input.prompt + '\x1f' + std::to_string(input.maxTokens);
  return visionHashBytesAndPrompt({.data = input.image.data,
                                   .len = static_cast<size_t>(input.image.total()) *
                                          input.image.elemSize(),
                                   .prompt = asked});
}

constexpr int32_t kFallbackMaxInputPx = 384;
constexpr int32_t kFallbackMaxTokens = 64;
constexpr int32_t kFallbackCacheSlots = 8;
constexpr int32_t kFallbackContextSize = 8192;

void mtmdDeleter(mtmd_context* ctx)
{
  if (ctx)
    mtmd_free(ctx);
}

}

VisionDefaults resolveVisionDefaults()
{
  const int32_t maxInputPx = ConfigService::getInt("vision.max_input_px");
  const int32_t maxTokens = ConfigService::getInt("vision.max_tokens");
  std::string prompt = ConfigService::getString("vision.prompt");
  const int32_t cacheSlots = ConfigService::getInt("vision.caption_cache_slots");
  return {.maxInputPx = maxInputPx < 128 ? kFallbackMaxInputPx : maxInputPx,
          .maxTokens = maxTokens <= 0 ? kFallbackMaxTokens : std::clamp(maxTokens, 8, 512),
          .prompt = prompt.empty() ? std::string(kFallbackPrompt) : std::move(prompt),
          .cacheSlots = cacheSlots <= 0 ? kFallbackCacheSlots : cacheSlots};
}

VisionEngineSettings resolveVisionEngineSettings()
{
  const int32_t contextSize = ConfigService::getInt("vision.context_size");
  return {.threads = std::max(0, ConfigService::getInt("vision.threads")),
          .gpuLayers = ConfigService::hasKey("vision.gpu_layers")
                           ? std::max(-1, ConfigService::getInt("vision.gpu_layers"))
                           : -1,
          .contextSize = contextSize < 2048 ? kFallbackContextSize : contextSize,
          .imageMaxTokens = std::max(0, ConfigService::getInt("vision.image_max_tokens"))};
}

VisionModelFiles resolveVisionModelFiles()
{
  std::string model = ConfigService::getString("vision.model_path");
  std::string mmproj = ConfigService::getString("vision.mmproj_path");
  return {.model = model.empty() ? std::string(kDefaultModel) : std::move(model),
          .mmproj = mmproj.empty() ? std::string(kDefaultMmproj) : std::move(mmproj)};
}

bool VisionModelFiles::present() const
{
  std::error_code error;
  return std::filesystem::is_regular_file(model, error) && std::filesystem::is_regular_file(mmproj, error);
}

VisionService::VisionService()
    : model_(nullptr, llama_model_free), context_(nullptr, llama_free),
      mtmd_(nullptr, mtmdDeleter), defaultPrompt_(kFallbackPrompt)
{
}

VisionService::~VisionService()
{
  shutdown();
}

void VisionService::init()
{
  try {
    std::scoped_lock lock(ai_init::llamaMutex());

    if (!HardwareProbe::vlmEnabled()) {
      LOG_WARN << "Vision: disabled on tier "
               << toString(HardwareProbe::get().tier);
      return;
    }

    const auto [modelPath, mmprojPath] = resolveVisionModelFiles();

    const VisionEngineSettings engine = resolveVisionEngineSettings();
    const int threads = engine.threads > 0 ? engine.threads : ThreadBudget::computeThreads();
    const int gpuLayers = engine.gpuLayers >= 0 ? engine.gpuLayers : HardwareProbe::vlmGpuLayers();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpuLayers;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;

    llama_model* rawModel = llama_model_load_from_file(modelPath.c_str(), mp);
    if (!rawModel)
      throw std::runtime_error("failed to load VLM: " + modelPath);
    model_.reset(rawModel);

    const int contextSize = engine.contextSize;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(contextSize);
    cp.n_batch = 512;
    cp.n_ubatch = 512;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    cp.offload_kqv = gpuLayers > 0;

    llama_context* rawCtx = llama_init_from_model(model_.get(), cp);
    if (!rawCtx)
      throw std::runtime_error("failed to create VLM context");
    context_.reset(rawCtx);
    nBatch_ = static_cast<int32_t>(cp.n_batch);

    auto params = mtmd_context_params_default();
    params.use_gpu = gpuLayers > 0;
    params.print_timings = false;
    params.n_threads = threads;
    params.warmup = true;
    if (engine.imageMaxTokens > 0)
      params.image_max_tokens = engine.imageMaxTokens;

    mtmd_context* rawMtmd =
        mtmd_init_from_file(mmprojPath.c_str(), model_.get(), params);
    if (!rawMtmd)
      throw std::runtime_error("failed to load mmproj: " + mmprojPath);
    mtmd_.reset(rawMtmd);

    if (!mtmd_support_vision(mtmd_.get()))
      throw std::runtime_error("mmproj has no vision encoder");

    {
      std::scoped_lock defaultsLock(mutex_);
      loadDefaults();
    }

    loaded_ = true;
    LOG_INFO << "Vision loaded: " << modelPath
             << " + mmproj (threads=" << threads << ", ctx=" << contextSize
             << ", max_input_px=" << maxInputPx() << ", gpu_layers=" << gpuLayers
             << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "Vision init failed: " << e.what();
    shutdown();
  }
}

void VisionService::shutdown()
{
  std::scoped_lock lock(mutex_);
  mtmd_.reset();
  context_.reset();
  model_.reset();
  cache_.clear();
  loaded_ = false;
  LOG_INFO << "Vision shutdown";
}

void VisionService::refreshDefaults()
{
  std::scoped_lock lock(mutex_);
  loadDefaults();
}

void VisionService::loadDefaults()
{
  VisionDefaults defaults = resolveVisionDefaults();
  maxInputPx_.store(defaults.maxInputPx, std::memory_order_relaxed);
  defaultMaxTokens_.store(defaults.maxTokens, std::memory_order_relaxed);
  defaultPrompt_ = std::move(defaults.prompt);
  cache_.assign(static_cast<size_t>(defaults.cacheSlots), CacheEntry{});
  cacheNext_ = 0;
}

bool VisionService::isLoaded() const
{
  return loaded_;
}

void VisionService::cancel()
{
  cancelled_.store(true, std::memory_order_relaxed);
}

const std::string* VisionService::cacheLookup(uint64_t key)
{
  for (const auto& e : cache_)
    if (e.key == key && !e.caption.empty())
      return &e.caption;
  return nullptr;
}

void VisionService::cacheStore(uint64_t key, const std::string& caption)
{
  if (cache_.empty() || caption.empty())
    return;
  cache_[cacheNext_] = CacheEntry{key, caption};
  cacheNext_ = (cacheNext_ + 1) % cache_.size();
}

cv::Mat VisionService::fitToBudget(const cv::Mat& src, bool srcIsBgr)
{
  const int maxInputPx = maxInputPx_.load(std::memory_order_relaxed);
  const int longest = std::max(src.cols, src.rows);
  cv::Mat scaled;

  if (longest > maxInputPx) {
    const double factor = static_cast<double>(maxInputPx) / longest;
    const cv::Size target(std::max(1, static_cast<int>(src.cols * factor)),
                          std::max(1, static_cast<int>(src.rows * factor)));
    if (longest >= 2 * maxInputPx) {
      cv::Mat mid;
      cv::resize(src, mid, cv::Size(target.width * 2, target.height * 2), 0, 0,
                 cv::INTER_AREA);
      cv::resize(mid, scaled, target, 0, 0, cv::INTER_LANCZOS4);
    }
    else {
      cv::resize(src, scaled, target, 0, 0, cv::INTER_LANCZOS4);
    }
  }
  else {
    scaled = src;
  }

  cv::Mat rgb;
  if (srcIsBgr)
    cv::cvtColor(scaled, rgb, cv::COLOR_BGR2RGB);
  else if (scaled.isContinuous())
    rgb = scaled;
  else
    rgb = scaled.clone();
  return rgb;
}

std::string VisionService::run(const VisionRunInput& input)
{
  const cv::Mat& src = input.src;
  if (!loaded_)
    throw ResponseException(VlmErrors::VisionEngineNotLoaded);
  if (src.empty())
    return "";

  std::scoped_lock lock(mutex_);
  if (!loaded_)
    throw ResponseException(VlmErrors::VisionEngineNotLoaded);
  cancelled_.store(false, std::memory_order_relaxed);

  const std::string question =
      input.prompt.empty() ? defaultPrompt_ : input.prompt;
  const int32_t maxTokens =
      input.maxTokens > 0 ? input.maxTokens : defaultMaxTokens();

  const cv::Mat rgb = fitToBudget(src, input.srcIsBgr);
  const uint64_t key = captionKey({.image = rgb, .prompt = question, .maxTokens = maxTokens});
  if (const std::string* hit = cacheLookup(key))
    return *hit;

  auto* ctx = context_.get();
  auto* mctx = mtmd_.get();
  auto mem = llama_get_memory(ctx);
  if (mem)
    llama_memory_clear(mem, true);

  const std::string prompt = std::string("<|im_start|>user\n") +
                             mtmd_default_marker() + "\n" + question +
                             "<|im_end|>\n<|im_start|>assistant\n";

  mtmd::bitmap bitmap(static_cast<uint32_t>(rgb.cols),
                      static_cast<uint32_t>(rgb.rows), rgb.data);
  mtmd::input_chunks chunks(mtmd_input_chunks_init());

  mtmd_input_text text{};
  text.text = prompt.c_str();
  text.text_len = prompt.size();
  text.add_special = true;
  text.parse_special = true;

  std::array<const mtmd_bitmap*, 1> bitmaps{bitmap.ptr.get()};
  if (mtmd_tokenize(mctx, chunks.ptr.get(), &text, bitmaps.data(), bitmaps.size()) != 0) {
    LOG_ERROR << "Vision: mtmd_tokenize failed";
    return "";
  }

  llama_pos nPast = 0;
  if (mtmd_helper_eval_chunks(mctx, ctx, chunks.ptr.get(), 0, 0, nBatch_, true,
                              &nPast) != 0) {
    LOG_ERROR << "Vision: image/prompt evaluation failed";
    return "";
  }

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
      smpl(llama_sampler_chain_init(sparams), &llama_sampler_free);
  if (!smpl) {
    LOG_ERROR << "Vision: failed to init sampler";
    return "";
  }
  llama_sampler_chain_add(smpl.get(), llama_sampler_init_greedy());

  const auto* vocab = llama_model_get_vocab(model_.get());
  const llama_token eos = llama_vocab_eos(vocab);
  const llama_token eot = llama_vocab_eot(vocab);

  std::string caption;
  struct BatchGuard
  {
    llama_batch value;
    explicit BatchGuard(llama_batch batch) : value(batch) {}
    ~BatchGuard() { llama_batch_free(value); }
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
  } batchGuard(llama_batch_init(1, 0, 1));
  llama_batch& batch = batchGuard.value;
  for (int32_t i = 0; i < maxTokens; ++i) {
    if (cancelled_.load(std::memory_order_relaxed))
      break;

    const llama_token token = llama_sampler_sample(smpl.get(), ctx, -1);
    if (token == eos || token == eot)
      break;

    std::array<char, 256> piece{};
    const int n = llama_token_to_piece(vocab, token, piece.data(),
                                       piece.size(), 0, true);
    if (n > 0)
      caption.append(piece.data(), static_cast<size_t>(n));

    batch.token[0] = token;
    batch.pos[0] = nPast++;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = 1;
    batch.n_tokens = 1;
    if (llama_decode(ctx, batch) != 0)
      break;
  }

  if (!cancelled_.load(std::memory_order_relaxed))
    cacheStore(key, caption);
  return caption;
}

std::string VisionService::describe(const VisionRequest& req)
{
  if (req.imageRgb.empty() || req.width == 0 || req.height == 0)
    return "";

  const cv::Mat rgb(static_cast<int>(req.height), static_cast<int>(req.width),
                    CV_8UC3, const_cast<unsigned char*>(req.imageRgb.data()));
  return run({.src = rgb,
              .srcIsBgr = false,
              .prompt = req.prompt,
              .maxTokens = req.maxTokens});
}

std::string VisionService::describeMat(const VisionDescribeMatInput& input)
{
  return run({.src = input.bgr,
              .srcIsBgr = true,
              .prompt = input.prompt,
              .maxTokens = input.maxTokens});
}

drogon::Task<std::string> VisionService::describeAsync(const VisionRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [this, req]() { return describe(req); }, BlockingLane::Heavy);
}

drogon::Task<std::string>
VisionService::describeMatAsync(const VisionDescribeMatInput& input)
{
  co_return co_await BlockingTask<std::string>(
      [this, input]() { return describeMat(input); },
      BlockingLane::Heavy);
}
