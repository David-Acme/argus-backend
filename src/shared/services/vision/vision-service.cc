#include "vision-service.hxx"

#include <algorithm>
#include <cstring>
#include <drogon/drogon.h>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>
#include <opencv2/imgproc.hpp>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/ai-init/ai-init.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>

namespace
{

constexpr const char* kDefaultModel = "models/vision/lfm2vl-25/lm-Q8_0.gguf";
constexpr const char* kDefaultMmproj =
    "models/vision/lfm2vl-25/mmproj-F16.gguf";
constexpr const char* kFallbackPrompt = "Can you describe this image?";

uint64_t hashBytes(const unsigned char* data, size_t len)
{
  uint64_t h = 14695981039346656037ULL;
  const size_t blocks = len / 8;
  const auto* words = reinterpret_cast<const uint64_t*>(data);
  for (size_t b = 0; b < blocks; ++b) {
    h ^= words[b];
    h *= 1099511628211ULL;
  }
  for (size_t i = blocks * 8; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

uint64_t hashMatAndPrompt(const cv::Mat& m, const std::string& prompt)
{
  uint64_t h = hashBytes(m.data, static_cast<size_t>(m.total()) * m.elemSize());
  h ^= hashBytes(reinterpret_cast<const unsigned char*>(prompt.data()),
                 prompt.size());
  h *= 1099511628211ULL;
  return h;
}

void mtmdDeleter(mtmd_context* ctx)
{
  if (ctx)
    mtmd_free(ctx);
}

} // namespace

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
    std::lock_guard<std::mutex> lock(ai_init::llamaMutex());

    if (!HardwareProbe::vlmEnabled()) {
      LOG_WARN << "Vision: disabled on tier "
               << toString(HardwareProbe::get().tier);
      return;
    }

    std::string modelPath = ConfigService::getString("vision.model_path");
    if (modelPath.empty())
      modelPath = kDefaultModel;
    std::string mmprojPath = ConfigService::getString("vision.mmproj_path");
    if (mmprojPath.empty())
      mmprojPath = kDefaultMmproj;

    int threads = ThreadBudget::computeThreads();
    if (const int cfg = ConfigService::getInt("vision.threads"); cfg > 0)
      threads = cfg;

    int gpuLayers = HardwareProbe::vlmGpuLayers();
    if (const int cfg = ConfigService::getInt("vision.gpu_layers"); cfg >= 0)
      gpuLayers = cfg;

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpuLayers;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;

    llama_model* rawModel = llama_model_load_from_file(modelPath.c_str(), mp);
    if (!rawModel)
      throw std::runtime_error("failed to load VLM: " + modelPath);
    model_.reset(rawModel);

    int contextSize = ConfigService::getInt("vision.context_size");
    if (contextSize < 2048)
      contextSize = 8192;

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
    if (const int v = ConfigService::getInt("vision.image_max_tokens"); v > 0)
      params.image_max_tokens = v;

    mtmd_context* rawMtmd =
        mtmd_init_from_file(mmprojPath.c_str(), model_.get(), params);
    if (!rawMtmd)
      throw std::runtime_error("failed to load mmproj: " + mmprojPath);
    mtmd_.reset(rawMtmd);

    if (!mtmd_support_vision(mtmd_.get()))
      throw std::runtime_error("mmproj has no vision encoder");

    defaultMaxTokens_ =
        std::clamp(ConfigService::getInt("vision.max_tokens"), 8, 512);
    maxInputPx_ = ConfigService::getInt("vision.max_input_px");
    if (maxInputPx_ < 128)
      maxInputPx_ = 384;
    defaultPrompt_ = ConfigService::getString("vision.prompt");
    if (defaultPrompt_.empty())
      defaultPrompt_ = kFallbackPrompt;

    int slots = ConfigService::getInt("vision.caption_cache_slots");
    if (slots <= 0)
      slots = 8;
    cache_.assign(static_cast<size_t>(slots), CacheEntry{});
    cacheNext_ = 0;

    loaded_ = true;
    LOG_INFO << "Vision loaded: " << modelPath
             << " + mmproj (threads=" << threads << ", ctx=" << contextSize
             << ", max_input_px=" << maxInputPx_ << ", gpu_layers=" << gpuLayers
             << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "Vision init failed: " << e.what();
    shutdown();
  }
}

void VisionService::shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  mtmd_.reset();
  context_.reset();
  model_.reset();
  cache_.clear();
  loaded_ = false;
  LOG_INFO << "Vision shutdown";
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
  const int longest = std::max(src.cols, src.rows);
  cv::Mat scaled;

  if (longest > maxInputPx_) {
    const double factor = static_cast<double>(maxInputPx_) / longest;
    const cv::Size target(std::max(1, static_cast<int>(src.cols * factor)),
                          std::max(1, static_cast<int>(src.rows * factor)));
    if (longest >= 2 * maxInputPx_) {
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

std::string VisionService::run(const cv::Mat& src, bool srcIsBgr,
                               const std::string& promptIn, int32_t maxTokensIn)
{
  if (!loaded_) {
    LOG_WARN << "Vision: service not loaded";
    return "";
  }
  if (src.empty())
    return "";

  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_.store(false, std::memory_order_relaxed);

  const std::string question = promptIn.empty() ? defaultPrompt_ : promptIn;
  const int32_t maxTokens = maxTokensIn > 0 ? maxTokensIn : defaultMaxTokens_;

  const cv::Mat rgb = fitToBudget(src, srcIsBgr);
  const uint64_t key = hashMatAndPrompt(rgb, question);
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

  const mtmd_bitmap* bitmaps[1] = {bitmap.ptr.get()};
  if (mtmd_tokenize(mctx, chunks.ptr.get(), &text, bitmaps, 1) != 0) {
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

    char piece[256];
    const int n =
        llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
    if (n > 0)
      caption.append(piece, static_cast<size_t>(n));

    llama_sampler_accept(smpl.get(), token);

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
  return run(rgb, false, req.prompt, req.maxTokens);
}

std::string VisionService::describeMat(const cv::Mat& bgr,
                                       const std::string& prompt,
                                       int32_t maxTokens)
{
  return run(bgr, true, prompt, maxTokens);
}

drogon::Task<std::string> VisionService::describeAsync(const VisionRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [this, req]() { return describe(req); });
}

drogon::Task<std::string>
VisionService::describeMatAsync(const cv::Mat& bgr, const std::string& prompt,
                                int32_t maxTokens)
{
  co_return co_await BlockingTask<std::string>(
      [this, bgr, prompt, maxTokens]() {
        return describeMat(bgr, prompt, maxTokens);
      });
}
