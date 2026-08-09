#include "embedding-service.hxx"

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

std::mutex gMutex;
std::unique_ptr<Ort::Env> gEnv;
std::unique_ptr<Ort::Session> gSession;
UnigramTokenizer gTokenizer;
int gMaxLen = 512;
int gDim = 384;
bool gLoaded = false;

std::vector<float> meanPool(const float* hidden, int seq, int dim,
                            const std::vector<int32_t>& ids)
{
  std::vector<float> pooled(static_cast<size_t>(dim), 0.0F);
  int count = 0;
  for (int i = 0; i < seq; ++i) {
    const bool pad = i >= static_cast<int>(ids.size()) ||
                     ids[static_cast<size_t>(i)] == gTokenizer.padId();
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

void EmbeddingService::init()
{
  std::lock_guard lock(gMutex);
  if (gLoaded)
    return;

  const std::string modelPath =
      ConfigService::getString("memory.embedding_model");
  const std::string tokenizerPath =
      ConfigService::getString("memory.embedding_tokenizer");
  const int maxLen = ConfigService::getInt("memory.embedding_max_len");
  if (maxLen > 0)
    gMaxLen = maxLen;

  if (!gTokenizer.load(tokenizerPath)) {
    LOG_WARN << "EmbeddingService: tokenizer not loaded ("
             << tokenizerPath << ")";
    return;
  }

  try {
    gEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "Argus-Embed");
    auto opts = Ort::SessionOptions{};
    opts.SetIntraOpNumThreads(ThreadBudget::lightThreads());
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    gSession = std::make_unique<Ort::Session>(*gEnv, modelPath.c_str(), opts);
    gDim = 384;
    gLoaded = true;
    LOG_INFO << "EmbeddingService: loaded (" << modelPath << ", dim=" << gDim
             << ", max_len=" << gMaxLen << ")";
  }
  catch (const std::exception& e) {
    LOG_WARN << "EmbeddingService: session init failed: " << e.what();
    gSession.reset();
    gEnv.reset();
  }
}

void EmbeddingService::shutdown()
{
  std::lock_guard lock(gMutex);
  gSession.reset();
  gEnv.reset();
  gLoaded = false;
}

bool EmbeddingService::isLoaded()
{
  std::lock_guard lock(gMutex);
  return gLoaded;
}

int EmbeddingService::dimensions()
{
  std::lock_guard lock(gMutex);
  return gDim;
}

std::optional<std::vector<float>> EmbeddingService::embed(
    const std::string& text, const std::string& prefix)
{
  std::lock_guard lock(gMutex);
  if (!gLoaded || !gSession)
    return std::nullopt;

  std::string input = prefix.empty() ? text : prefix + " " + text;
  const auto ids = gTokenizer.encode(input, gMaxLen);
  const int64_t seq = static_cast<int64_t>(ids.size());

  std::vector<int64_t> inputIds(ids.begin(), ids.end());
  std::vector<int64_t> attentionMask(ids.size(), 1);
  std::vector<int64_t> tokenTypes(ids.size(), 0);

  const std::array<int64_t, 2> shape{1, seq};
  auto inIds = Ort::Value::CreateTensor<int64_t>(Ort::MemoryInfo::CreateCpu(
                                                     OrtArenaAllocator, OrtMemTypeDefault),
                                                 inputIds.data(),
                                                 inputIds.size(), shape.data(),
                                                 shape.size());
  auto inMask = Ort::Value::CreateTensor<int64_t>(Ort::MemoryInfo::CreateCpu(
                                                      OrtArenaAllocator, OrtMemTypeDefault),
                                                  attentionMask.data(),
                                                  attentionMask.size(),
                                                  shape.data(), shape.size());
  auto inTypes = Ort::Value::CreateTensor<int64_t>(Ort::MemoryInfo::CreateCpu(
                                                       OrtArenaAllocator, OrtMemTypeDefault),
                                                   tokenTypes.data(),
                                                   tokenTypes.size(),
                                                   shape.data(), shape.size());

  const char* inputNames[] = {"input_ids", "attention_mask", "token_type_ids"};
  const char* outputNames[] = {"last_hidden_state"};
  Ort::RunOptions runOpts;

  try {
    auto outputs = gSession->Run(runOpts, inputNames,
                                 std::array<Ort::Value, 3>{std::move(inIds),
                                                           std::move(inMask),
                                                           std::move(inTypes)}
                                     .data(),
                                 3, outputNames, 1);
    const auto& out = outputs.front();
    const auto* hidden = out.GetTensorData<float>();
    const auto outShape = out.GetTensorTypeAndShapeInfo().GetShape();
    const int64_t outSeq = outShape.size() > 1 ? outShape[1] : seq;
    auto pooled = meanPool(hidden, static_cast<int>(outSeq), gDim, ids);
    if (pooled.empty())
      return std::nullopt;
    return pooled;
  }
  catch (const std::exception& e) {
    LOG_WARN << "EmbeddingService: run failed: " << e.what();
    return std::nullopt;
  }
}
