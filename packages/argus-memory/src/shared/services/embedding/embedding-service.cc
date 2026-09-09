#include "embedding-service.hxx"

#include <array>
#include <cmath>
#include <drogon/drogon.h>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/unigram-tokenizer.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <string>
#include <vector>

namespace
{

std::vector<float> meanPool(const float* hidden, int seq, int dim,
                            const std::vector<int32_t>& ids, int32_t padId)
{
  std::vector<float> pooled(static_cast<size_t>(dim), 0.0F);
  int count = 0;
  for (int i = 0; i < seq; ++i) {
    const bool pad = i >= static_cast<int>(ids.size()) ||
                     ids[static_cast<size_t>(i)] == padId;
    if (pad)
      continue;
    const float* row = hidden + static_cast<size_t>(i) * dim;
    for (int d = 0; d < dim; ++d)
      pooled[static_cast<size_t>(d)] += row[d];
    ++count;
  }
  if (count == 0)
    return {};

  float norm = 0.0F;
  for (float& v : pooled) {
    v /= static_cast<float>(count);
    norm += v * v;
  }
  norm = std::sqrt(norm);
  if (norm > 0.0F) {
    for (float& v : pooled)
      v /= norm;
  }
  return pooled;
}

} // namespace

EmbeddingService::EmbeddingService() = default;

EmbeddingService::~EmbeddingService()
{
  shutdown();
}

bool EmbeddingService::ensureLoadedLocked()
{
  if (loaded_)
    return true;
  if (initAttempted_)
    return false;

  initAttempted_ = true;
  const std::string modelPath =
      ConfigService::getString("memory.embedding_model");
  const std::string tokenizerPath =
      ConfigService::getString("memory.embedding_tokenizer");
  const int maxLen = ConfigService::getInt("memory.embedding_max_len");
  if (maxLen > 0)
    maxLen_ = maxLen;

  if (!tokenizer_.load(tokenizerPath)) {
    LOG_WARN << "EmbeddingService: tokenizer not loaded (" << tokenizerPath
             << ")";
    return false;
  }

  try {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Argus-Embed");
    auto opts = Ort::SessionOptions{};
    opts.SetIntraOpNumThreads(ThreadBudget::lightThreads());
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    session_ = std::make_unique<Ort::Session>(*env_, modelPath.c_str(), opts);
    dim_ = 384;
    const int configured = ConfigService::getInt("memory.embedding_dim");
    outDim_ = configured > 0 ? std::min(configured, dim_) : dim_;
    loaded_ = true;
    LOG_INFO << "EmbeddingService: loaded (" << modelPath
             << ", model_dim=" << dim_ << ", dim=" << outDim_
             << ", max_len=" << maxLen_ << ")";
  }
  catch (const std::exception& e) {
    LOG_WARN << "EmbeddingService: session init failed: " << e.what();
    session_.reset();
    env_.reset();
  }
  return loaded_;
}

void EmbeddingService::init()
{
  if (!ConfigService::getBool("memory.embedding_preload"))
    return;
  std::lock_guard lock(mutex_);
  ensureLoadedLocked();
}

void EmbeddingService::shutdown()
{
  std::lock_guard lock(mutex_);
  session_.reset();
  env_.reset();
  tokenizer_ = UnigramTokenizer{};
  loaded_ = false;
  initAttempted_ = false;
}

bool EmbeddingService::isLoaded() const
{
  std::lock_guard lock(mutex_);
  return loaded_;
}

int EmbeddingService::dimensions() const
{
  std::lock_guard lock(mutex_);
  return outDim_ > 0 ? outDim_ : dim_;
}

std::optional<std::vector<float>>
EmbeddingService::embed(const std::string& text, const std::string& prefix)
{
  std::lock_guard lock(mutex_);
  if (!ensureLoadedLocked())
    return std::nullopt;

  std::string input = prefix.empty() ? text : prefix + " " + text;
  const auto ids = tokenizer_.encode(input, maxLen_);
  const int64_t seq = static_cast<int64_t>(ids.size());

  std::vector<int64_t> inputIds(ids.begin(), ids.end());
  std::vector<int64_t> attentionMask(ids.size(), 1);
  std::vector<int64_t> tokenTypes(ids.size(), 0);

  const std::array<int64_t, 2> shape{1, seq};
  auto inIds = Ort::Value::CreateTensor<int64_t>(
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
      inputIds.data(), inputIds.size(), shape.data(), shape.size());
  auto inMask = Ort::Value::CreateTensor<int64_t>(
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
      attentionMask.data(), attentionMask.size(), shape.data(), shape.size());
  auto inTypes = Ort::Value::CreateTensor<int64_t>(
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
      tokenTypes.data(), tokenTypes.size(), shape.data(), shape.size());

  const std::array inputNames{"input_ids", "attention_mask", "token_type_ids"};
  const std::array outputNames{"last_hidden_state"};
  Ort::RunOptions runOpts;

  try {
    auto outputs = session_->Run(runOpts, inputNames.data(),
                                 std::array<Ort::Value, 3>{std::move(inIds),
                                                           std::move(inMask),
                                                           std::move(inTypes)}
                                     .data(),
                                 3, outputNames.data(), 1);
    const auto& out = outputs.front();
    const auto* hidden = out.GetTensorData<float>();
    const auto outShape = out.GetTensorTypeAndShapeInfo().GetShape();
    const int64_t outSeq = outShape.size() > 1 ? outShape[1] : seq;
    auto pooled = meanPool(hidden, static_cast<int>(outSeq), dim_, ids,
                           tokenizer_.padId());
    if (pooled.empty())
      return std::nullopt;
    if (outDim_ < dim_)
      pooled.resize(static_cast<size_t>(outDim_));
    return pooled;
  }
  catch (const std::exception& e) {
    LOG_WARN << "EmbeddingService: run failed: " << e.what();
    return std::nullopt;
  }
}
