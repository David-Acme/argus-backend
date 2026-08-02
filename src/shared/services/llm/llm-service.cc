#include "llm-service.hxx"

#include <cstdint>
#include <cstring>
#include <drogon/drogon.h>
#include <llama.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <thread>
#include <vector>

std::unique_ptr<llama_model, void (*)(llama_model*)>
    LlmService::model_{nullptr, llama_model_free};
std::unique_ptr<llama_context, void (*)(llama_context*)>
    LlmService::context_{nullptr, llama_free};
int64_t LlmService::contextSize_ = 0;
int32_t LlmService::defaultMaxTokens_ = 96;
float LlmService::defaultTemperature_ = 0.3F;
bool LlmService::loaded_ = false;
std::mutex LlmService::mutex_;

void LlmService::init()
{
  try {
    llama_log_set(
        [](enum ggml_log_level level, const char* text, void*) {
          if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR) {
            fputs(text, stderr);
          }
        },
        nullptr);

    const std::string modelPath =
        ConfigService::getString("llm.model_path").empty()
            ? "models/llm/LFM2.5-350M-Q4_K_M.gguf"
            : ConfigService::getString("llm.model_path");
    const int64_t contextSize =
        std::clamp<int64_t>(ConfigService::getInt("llm.context_size"), 4096,
                            128000);

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = 0;
    modelParams.use_mmap = true;

    llama_model* rawModel =
        llama_model_load_from_file(modelPath.c_str(), modelParams);
    if (!rawModel) {
      throw std::runtime_error("failed to load model: " + modelPath);
    }
    model_.reset(rawModel);

    auto nThreads = ThreadBudget::lightThreads();
    auto nThreadsBatch = ThreadBudget::batchThreads();
    if (const int cfg = ConfigService::getInt("llm.threads"); cfg > 0)
      nThreads = cfg;
    if (const int cfg = ConfigService::getInt("llm.batch_threads"); cfg > 0)
      nThreadsBatch = cfg;

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = static_cast<uint32_t>(contextSize);
    ctxParams.n_batch =
        std::max(256, ConfigService::getInt("llm.n_batch"));
    ctxParams.n_ubatch =
        std::max(256, ConfigService::getInt("llm.n_ubatch"));
    ctxParams.n_threads = nThreads;
    ctxParams.n_threads_batch = nThreadsBatch;
    ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
    ctxParams.type_k = GGML_TYPE_Q4_0;
    ctxParams.type_v = GGML_TYPE_Q4_0;
    ctxParams.no_perf = true;
    ctxParams.offload_kqv = false;
    ctxParams.swa_full = true;

    llama_context* rawCtx = llama_init_from_model(model_.get(), ctxParams);
    if (!rawCtx) {
      throw std::runtime_error("failed to create context");
    }
    context_.reset(rawCtx);

    warmup();

    contextSize_ = contextSize;
    defaultMaxTokens_ =
        std::clamp<int32_t>(ConfigService::getInt("llm.max_tokens"), 16, 4096);
    defaultTemperature_ = static_cast<float>(
        std::clamp(ConfigService::getDouble("llm.temperature"), 0.0, 2.0));
    loaded_ = true;

    LOG_INFO << "LLM loaded: " << modelPath << " (ctx=" << contextSize
             << ", threads=" << nThreads << ", batch_threads=" << nThreadsBatch
             << ", K=V=Q4_0)";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "LLM init failed: " << e.what();
    shutdown();
  }
}

void LlmService::shutdown()
{
  context_.reset();
  model_.reset();
  loaded_ = false;

  LOG_INFO << "LLM shutdown";
}

bool LlmService::isLoaded()
{
  return loaded_;
}

void LlmService::warmup()
{
  auto* ctx = context_.get();
  auto* vocab = llama_model_get_vocab(model_.get());

  constexpr const char* text =
      "Hola, esto es una prueba de calentamiento del modelo.";
  const int len = static_cast<int>(std::strlen(text));
  std::vector<llama_token> tokens(static_cast<size_t>(len) * 2);

  int n = llama_tokenize(vocab, text, len, tokens.data(),
                         static_cast<int32_t>(tokens.size()), true, true);
  if (n < 0)
    return;
  tokens.resize(static_cast<size_t>(n));

  auto batch = llama_batch_get_one(tokens.data(), n);
  llama_decode(ctx, batch);
  llama_memory_seq_rm(llama_get_memory(ctx), 0, 0, -1);
}

std::string LlmService::buildPrompt(const std::vector<ChatMessage>& messages)
{
  std::string prompt;

  bool hasSystem = false;
  for (const auto& msg : messages) {
    if (msg.role == "system") {
      hasSystem = true;
      break;
    }
  }

  if (!hasSystem) {
    prompt += "<|im_start|>system\n";
    prompt += SYSTEM_PROMPT;
    prompt += "<|im_end|>\n";
  }

  for (const auto& msg : messages) {
    if (msg.role == "system") {
      prompt += "<|im_start|>system\n";
      prompt += msg.content;
      prompt += "<|im_end|>\n";
    }
    else if (msg.role == "user") {
      prompt += "<|im_start|>user\n";
      prompt += msg.content;
      prompt += "<|im_end|>\n";
    }
    else if (msg.role == "assistant") {
      prompt += "<|im_start|>assistant\n";
      prompt += msg.content;
      prompt += "<|im_end|>\n";
    }
  }

  prompt += "<|im_start|>assistant\n";
  return prompt;
}

void LlmService::generateStream(const std::string& formattedPrompt,
                                float temperature, int32_t maxTokens,
                                bool resetContext, TokenCallback onToken)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto* ctx = context_.get();
  auto* model = model_.get();
  auto* vocab = llama_model_get_vocab(model);

  if (resetContext) {
    auto mem = llama_get_memory(ctx);
    if (mem) {
      llama_memory_seq_rm(mem, 0, 0, -1);
    }
  }

  auto promptLen = static_cast<int32_t>(formattedPrompt.size());
  std::vector<llama_token> promptTokens(static_cast<size_t>(promptLen) * 2);

  int nTokens =
      llama_tokenize(vocab, formattedPrompt.c_str(), promptLen,
                     promptTokens.data(),
                     static_cast<int32_t>(promptTokens.size()), true, true);

  if (nTokens < 0) {
    if (nTokens == INT32_MIN) {
      LOG_WARN << "LLM: token count overflow";
      onToken("", true);
      return;
    }
    auto needed = static_cast<size_t>(-nTokens);
    promptTokens.resize(needed);
    nTokens = llama_tokenize(vocab, formattedPrompt.c_str(), promptLen,
                             promptTokens.data(), static_cast<int32_t>(needed),
                             true, true);
  }

  if (nTokens < 0) {
    LOG_WARN << "LLM: tokenization failed (code=" << nTokens << ")";
    onToken("", true);
    return;
  }

  promptTokens.resize(static_cast<size_t>(nTokens));

  auto promptBatch = llama_batch_init(nTokens, 0, 1);
  for (int32_t j = 0; j < nTokens; ++j) {
    promptBatch.token[j] = promptTokens[j];
    promptBatch.pos[j] = j;
    promptBatch.n_seq_id[j] = 1;
    promptBatch.seq_id[j][0] = 0;
    promptBatch.logits[j] = (j == nTokens - 1) ? 1 : 0;
  }
  promptBatch.n_tokens = nTokens;

  if (llama_decode(ctx, promptBatch) != 0) {
    LOG_WARN << "LLM: prompt decode failed";
    llama_batch_free(promptBatch);
    onToken("", true);
    return;
  }
  llama_batch_free(promptBatch);

  auto nVocab = llama_vocab_n_tokens(vocab);
  if (nVocab <= 0) {
    LOG_WARN << "LLM: invalid vocab size: " << nVocab;
    onToken("", true);
    return;
  }

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  auto* smpl = llama_sampler_chain_init(sparams);
  if (!smpl) {
    LOG_WARN << "LLM: failed to init sampler chain";
    onToken("", true);
    return;
  }

  llama_sampler_chain_add(smpl, llama_sampler_init_temp(temperature));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_k(20));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.8f, 1));
  llama_sampler_chain_add(smpl,
                          llama_sampler_init_penalties(64, 1.1f, 1.2f, 0.0f));
  llama_sampler_chain_add(smpl, llama_sampler_init_dist(42));

  llama_token eosToken = llama_vocab_eos(vocab);
  llama_token eotToken = llama_vocab_eot(vocab);
  llama_pos pos = nTokens;

  auto genBatch = llama_batch_init(1, 0, 1);
  for (int32_t i = 0; i < maxTokens; ++i) {
    llama_token newToken = llama_sampler_sample(smpl, ctx, -1);

    if (newToken == eosToken || newToken == eotToken)
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, newToken, buf, sizeof(buf), 0, true);
    if (n > 0)
      onToken(std::string(buf, n), false);

    llama_sampler_accept(smpl, newToken);

    genBatch.token[0] = newToken;
    genBatch.pos[0] = pos++;
    genBatch.n_seq_id[0] = 1;
    genBatch.seq_id[0][0] = 0;
    genBatch.logits[0] = 1;
    genBatch.n_tokens = 1;
    if (llama_decode(ctx, genBatch) != 0)
      break;
  }
  llama_batch_free(genBatch);

  llama_sampler_free(smpl);
  onToken("", true);
}

std::string LlmService::generate(const std::string& formattedPrompt,
                                 float temperature, int32_t maxTokens,
                                 bool resetContext)
{
  std::string result;
  generateStream(formattedPrompt, temperature, maxTokens, resetContext,
                 [&result](const std::string& token, bool done) {
                   if (!done)
                     result.append(token);
                 });
  return result;
}

std::string LlmService::chat(const ChatRequest& req)
{
  std::string prompt = buildPrompt(req.messages);
  const int32_t maxTokens = req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
  const float temp = req.temperature >= 0.0F ? req.temperature
                                             : defaultTemperature_;
  return generate(prompt, temp, maxTokens, req.resetContext);
}

void LlmService::chatStream(const ChatRequest& req, TokenCallback onToken)
{
  std::string prompt = buildPrompt(req.messages);
  const int32_t maxTokens = req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
  const float temp = req.temperature >= 0.0F ? req.temperature
                                             : defaultTemperature_;
  generateStream(prompt, temp, maxTokens, req.resetContext,
                 std::move(onToken));
}

drogon::Task<std::string> LlmService::chatAsync(const ChatRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [req]() { return LlmService::chat(req); });
}

drogon::Task<void> LlmService::chatStreamAsync(const ChatRequest& req,
                                               TokenCallback onToken)
{
  co_await BlockingTask<void>([req, onToken = std::move(onToken)]() mutable {
    auto wrapped = [callback = std::move(onToken)](const std::string& token,
                                                   bool done) {
      drogon::app().getLoop()->queueInLoop(
          [callback, token, done]() { callback(token, done); });
    };
    std::string prompt = buildPrompt(req.messages);
    const int32_t maxTokens =
        req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
    const float temp = req.temperature >= 0.0F ? req.temperature
                                               : defaultTemperature_;
    generateStream(prompt, temp, maxTokens, req.resetContext,
                   std::move(wrapped));
  });
  co_return;
}
