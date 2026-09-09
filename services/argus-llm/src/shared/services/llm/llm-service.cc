#include <shared/services/llm/llm-service.hxx>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <drogon/drogon.h>
#include <llama.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/ai-init/ai-init.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/hardware-profile/hardware-profile.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <thread>
#include <vector>

namespace
{

ggml_type kvTypeFromConfig(const std::string& name)
{
  if (name == "q8_0")
    return GGML_TYPE_Q8_0;
  if (name == "q4_0")
    return GGML_TYPE_Q4_0;
  return GGML_TYPE_F16;
}

llama_flash_attn_type flashAttnFromConfig(const std::string& name)
{
  if (name == "on")
    return LLAMA_FLASH_ATTN_TYPE_ENABLED;
  if (name == "off")
    return LLAMA_FLASH_ATTN_TYPE_DISABLED;
  return LLAMA_FLASH_ATTN_TYPE_AUTO;
}

} // namespace

LlmService::LlmService()
    : model_(nullptr, llama_model_free), context_(nullptr, llama_free)
{
  seed_ = LLAMA_DEFAULT_SEED;
}

LlmService::~LlmService()
{
  shutdown();
}

LlmService& LlmService::instance()
{
  static LlmService service;
  return service;
}

void LlmService::init()
{
  try {
    std::lock_guard<std::mutex> lock(ai_init::llamaMutex());

    llama_log_set(
        [](enum ggml_log_level level, const char* text, void*) {
          if (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR)
            return;
          std::string line(text ? text : "");
          while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
          if (line.empty())
            return;
          if (level == GGML_LOG_LEVEL_ERROR)
            LOG_ERROR << "llama: " << line;
          else
            LOG_WARN << "llama: " << line;
        },
        nullptr);

    const std::string modelPath =
        ConfigService::getString("llm.model_path").empty()
            ? "models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf"
            : ConfigService::getString("llm.model_path");
    const int64_t contextSize =
        std::clamp<int64_t>(ConfigService::getInt("llm.context_size"), 4096,
                            128000);

    int gpuLayers = HardwareProbe::llmGpuLayers();
    if (const int cfg = ConfigService::getInt("llm.gpu_layers"); cfg >= 0)
      gpuLayers = cfg;

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = gpuLayers;
    modelParams.load_mode = LLAMA_LOAD_MODE_MMAP;

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

    std::string kvTypeName = ConfigService::getString("llm.kv_type");
    if (kvTypeName.empty() || kvTypeName == "auto")
      kvTypeName = HardwareProbe::llmKvType();
    const std::string flashAttnName =
        ConfigService::getString("llm.flash_attn").empty()
            ? "auto"
            : ConfigService::getString("llm.flash_attn");

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = static_cast<uint32_t>(contextSize);
    ctxParams.n_batch = std::max(256, ConfigService::getInt("llm.n_batch"));
    ctxParams.n_ubatch = std::max(256, ConfigService::getInt("llm.n_ubatch"));
    ctxParams.n_threads = nThreads;
    ctxParams.n_threads_batch = nThreadsBatch;
    ctxParams.flash_attn_type = flashAttnFromConfig(flashAttnName);
    ctxParams.type_k = kvTypeFromConfig(kvTypeName);
    ctxParams.type_v = kvTypeFromConfig(kvTypeName);
    ctxParams.no_perf = true;
    ctxParams.offload_kqv = gpuLayers > 0;
    ctxParams.swa_full = ConfigService::getBool("llm.swa_full");

    llama_context* rawCtx = llama_init_from_model(model_.get(), ctxParams);
    if (!rawCtx) {
      throw std::runtime_error("failed to create context");
    }
    context_.reset(rawCtx);

    nBatch_ = static_cast<int32_t>(ctxParams.n_batch);
    promptBatch_ =
        std::make_unique<llama_batch>(llama_batch_init(nBatch_, 0, 1));
    genBatch_ = std::make_unique<llama_batch>(llama_batch_init(1, 0, 1));

    if (const char* tmpl = llama_model_chat_template(model_.get(), nullptr))
      chatTemplate_ = tmpl;
    const std::string tmplMode = ConfigService::getString("llm.chat_template");
    if (tmplMode == "chatml")
      chatTemplate_.clear();

    contextSize_ = contextSize;
    defaultMaxTokens_ =
        std::clamp<int32_t>(ConfigService::getInt("llm.max_tokens"), 16, 4096);
    defaultTemperature_ = static_cast<float>(
        std::clamp(ConfigService::getDouble("llm.temperature"), 0.0, 2.0));

    if (const int v = ConfigService::getInt("llm.top_k"); v > 0)
      topK_ = v;
    if (const double v = ConfigService::getDouble("llm.top_p"); v > 0.0)
      topP_ = static_cast<float>(v);
    if (const int v = ConfigService::getInt("llm.penalty_last_n"); v > 0)
      penaltyLastN_ = v;
    if (const double v = ConfigService::getDouble("llm.penalty_repeat");
        v > 0.0)
      penaltyRepeat_ = static_cast<float>(v);
    penaltyFreq_ = static_cast<float>(
        std::max(0.0, ConfigService::getDouble("llm.penalty_freq")));
    penaltyPresent_ = static_cast<float>(
        std::max(0.0, ConfigService::getDouble("llm.penalty_present")));
    if (const int v = ConfigService::getInt("llm.seed"); v > 0)
      seed_ = static_cast<uint32_t>(v);

    warmup();
    loaded_ = true;

    LOG_INFO << "LLM loaded: " << modelPath << " (ctx=" << contextSize
             << ", n_batch=" << nBatch_ << ", threads=" << nThreads
             << ", batch_threads=" << nThreadsBatch << ", KV=" << kvTypeName
             << ", gpu_layers=" << gpuLayers << ", template="
             << (chatTemplate_.empty() ? "chatml-fallback" : "from-model")
             << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "LLM init failed: " << e.what();
    shutdown();
  }
}

void LlmService::shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (promptBatch_) {
    llama_batch_free(*promptBatch_);
    promptBatch_.reset();
  }
  if (genBatch_) {
    llama_batch_free(*genBatch_);
    genBatch_.reset();
  }
  cachedTokens_.clear();
  context_.reset();
  model_.reset();
  loaded_ = false;

  LOG_INFO << "LLM shutdown";
}

bool LlmService::isLoaded()
{
  return loaded_;
}

bool LlmService::isBusy()
{
  return busy_.load();
}

LlmPrefillStats LlmService::lastPrefillStats()
{
  return lastStats_;
}

std::vector<int32_t> LlmService::tokenize(const std::string& text,
                                          bool addSpecial)
{
  auto* vocab = llama_model_get_vocab(model_.get());
  const auto len = static_cast<int32_t>(text.size());

  std::vector<int32_t> tokens(static_cast<size_t>(len) / 3 + 16);
  int n = llama_tokenize(vocab, text.c_str(), len, tokens.data(),
                         static_cast<int32_t>(tokens.size()), addSpecial, true);
  if (n < 0) {
    if (n == INT32_MIN)
      return {};
    tokens.resize(static_cast<size_t>(-n));
    n = llama_tokenize(vocab, text.c_str(), len, tokens.data(),
                       static_cast<int32_t>(tokens.size()), addSpecial, true);
  }
  if (n < 0)
    return {};
  tokens.resize(static_cast<size_t>(n));
  return tokens;
}

bool LlmService::prefill(const std::vector<int32_t>& promptTokens,
                         bool forceReset)
{
  auto* ctx = context_.get();
  auto mem = llama_get_memory(ctx);

  size_t reuse = 0;
  if (!forceReset) {
    const size_t limit = std::min(cachedTokens_.size(), promptTokens.size());
    while (reuse < limit && cachedTokens_[reuse] == promptTokens[reuse])
      ++reuse;
    if (reuse != cachedTokens_.size())
      reuse = 0;
    if (reuse == promptTokens.size() && reuse > 0)
      --reuse;
  }

  if (reuse == 0 && mem)
    llama_memory_clear(mem, true);

  lastStats_.promptTokens = static_cast<int32_t>(promptTokens.size());
  lastStats_.reusedTokens = static_cast<int32_t>(reuse);
  lastStats_.decodedTokens = static_cast<int32_t>(promptTokens.size() - reuse);

  auto& batch = *promptBatch_;
  const size_t total = promptTokens.size();

  for (size_t start = reuse; start < total;
       start += static_cast<size_t>(nBatch_)) {
    const size_t count = std::min(static_cast<size_t>(nBatch_), total - start);
    const bool lastChunk = (start + count) >= total;

    for (size_t j = 0; j < count; ++j) {
      const size_t pos = start + j;
      batch.token[j] = promptTokens[pos];
      batch.pos[j] = static_cast<int32_t>(pos);
      batch.n_seq_id[j] = 1;
      batch.seq_id[j][0] = 0;
      batch.logits[j] = (lastChunk && j == count - 1) ? 1 : 0;
    }
    batch.n_tokens = static_cast<int32_t>(count);

    if (llama_decode(ctx, batch) != 0) {
      LOG_WARN << "LLM: prompt decode failed at offset " << start;
      cachedTokens_.clear();
      if (mem)
        llama_memory_clear(mem, true);
      return false;
    }
  }

  cachedTokens_ = promptTokens;
  return true;
}

void LlmService::warmup()
{
  const std::vector<ChatMessage> msgs = {{"user", "Hola."}};
  const auto tokens = tokenize(buildPrompt(msgs), true);
  if (tokens.empty())
    return;

  if (!prefill(tokens, true))
    return;

  auto* ctx = context_.get();
  auto& batch = *genBatch_;
  batch.token[0] = tokens.back();
  batch.pos[0] = static_cast<int32_t>(tokens.size());
  batch.n_seq_id[0] = 1;
  batch.seq_id[0][0] = 0;
  batch.logits[0] = 1;
  batch.n_tokens = 1;
  llama_decode(ctx, batch);

  if (auto mem = llama_get_memory(ctx))
    llama_memory_clear(mem, true);
  cachedTokens_.clear();
}

std::string
LlmService::buildChatMlPrompt(const std::vector<ChatMessage>& messages)
{
  std::string prompt;
  for (const auto& msg : messages) {
    prompt += "<|im_start|>";
    prompt += msg.role;
    prompt += "\n";
    prompt += msg.content;
    prompt += "<|im_end|>\n";
  }
  prompt += "<|im_start|>assistant\n";
  return prompt;
}

std::string LlmService::buildPrompt(const std::vector<ChatMessage>& messages)
{
  if (chatTemplate_.empty())
    return buildChatMlPrompt(messages);

  std::vector<llama_chat_message> chat;
  chat.reserve(messages.size());
  for (const auto& msg : messages)
    chat.push_back({msg.role.c_str(), msg.content.c_str()});

  size_t approx = 64;
  for (const auto& msg : messages)
    approx += msg.role.size() + msg.content.size() + 32;

  std::vector<char> buf(approx);
  int32_t n = llama_chat_apply_template(chatTemplate_.c_str(), chat.data(),
                                        chat.size(), true, buf.data(),
                                        static_cast<int32_t>(buf.size()));
  if (n > static_cast<int32_t>(buf.size())) {
    buf.resize(static_cast<size_t>(n));
    n = llama_chat_apply_template(chatTemplate_.c_str(), chat.data(),
                                  chat.size(), true, buf.data(),
                                  static_cast<int32_t>(buf.size()));
  }
  if (n <= 0) {
    LOG_WARN << "LLM: chat template failed, falling back to ChatML";
    return buildChatMlPrompt(messages);
  }
  return std::string(buf.data(), static_cast<size_t>(n));
}

void LlmService::generateStream(const std::string& formattedPrompt,
                                float temperature, int32_t maxTokens,
                                bool resetContext,
                                const std::vector<std::string>& stop,
                                TokenCallback onToken)
{
  std::lock_guard<std::mutex> lock(mutex_);
  struct BusyGuard
  {
    ~BusyGuard() { owner->busy_.store(false); }
    LlmService* owner;
  } busyGuard{this};
  busy_.store(true);

  auto* ctx = context_.get();
  auto* model = model_.get();
  auto* vocab = llama_model_get_vocab(model);

  const auto promptTokens = tokenize(formattedPrompt, true);
  if (promptTokens.empty()) {
    LOG_WARN << "LLM: tokenization failed";
    onToken("", true);
    return;
  }

  if (static_cast<int64_t>(promptTokens.size()) + maxTokens >= contextSize_) {
    LOG_WARN << "LLM: prompt (" << promptTokens.size()
             << " tokens) + max_tokens exceeds context " << contextSize_;
    onToken("", true);
    return;
  }

  if (!prefill(promptTokens, resetContext)) {
    onToken("", true);
    return;
  }

  auto nVocab = llama_vocab_n_tokens(vocab);
  if (nVocab <= 0) {
    LOG_WARN << "LLM: invalid vocab size: " << nVocab;
    onToken("", true);
    return;
  }

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  std::unique_ptr<llama_sampler, void (*)(llama_sampler*)>
      smpl(llama_sampler_chain_init(sparams), &llama_sampler_free);
  if (!smpl) {
    LOG_WARN << "LLM: failed to init sampler chain";
    onToken("", true);
    return;
  }

  llama_sampler_chain_add(smpl.get(),
                          llama_sampler_init_penalties(nVocab, penaltyLastN_,
                                                       penaltyRepeat_,
                                                       penaltyFreq_,
                                                       penaltyPresent_));
  llama_sampler_chain_add(smpl.get(), llama_sampler_init_top_k(topK_));
  llama_sampler_chain_add(smpl.get(), llama_sampler_init_top_p(topP_, 1));
  llama_sampler_chain_add(smpl.get(), llama_sampler_init_temp(temperature));
  llama_sampler_chain_add(smpl.get(), llama_sampler_init_dist(seed_));

  const llama_token eosToken = llama_vocab_eos(vocab);
  const llama_token eotToken = llama_vocab_eot(vocab);
  llama_pos pos = static_cast<llama_pos>(promptTokens.size());

  auto& batch = *genBatch_;
  // Only the tail can carry a stop match, and a match ends the generation
  // with its text already emitted.
  std::string tail;
  size_t tailKeep = 0;
  for (const auto& needle : stop)
    tailKeep = std::max(tailKeep, needle.size());
  tailKeep += 256;
  for (int32_t i = 0; i < maxTokens; ++i) {
    const llama_token newToken = llama_sampler_sample(smpl.get(), ctx, -1);

    if (newToken == eosToken || newToken == eotToken)
      break;

    std::array<char, 256> buf{};
    const int n = llama_token_to_piece(vocab, newToken, buf.data(),
                                       buf.size(), 0, true);
    if (n > 0) {
      const std::string piece(buf.data(), static_cast<size_t>(n));
      onToken(piece, false);
      if (!stop.empty()) {
        tail += piece;
        bool hit = false;
        for (const auto& needle : stop) {
          if (tail.find(needle) != std::string::npos)
            hit = true;
        }
        if (hit)
          break;
        if (tail.size() > tailKeep)
          tail.erase(0, tail.size() - tailKeep);
      }
    }

    llama_sampler_accept(smpl.get(), newToken);
    cachedTokens_.push_back(newToken);

    batch.token[0] = newToken;
    batch.pos[0] = pos++;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = 1;
    batch.n_tokens = 1;
    if (llama_decode(ctx, batch) != 0)
      break;
  }

  onToken("", true);
}

std::string LlmService::generate(const std::string& formattedPrompt,
                                 float temperature, int32_t maxTokens,
                                 bool resetContext,
                                 const std::vector<std::string>& stop)
{
  std::string result;
  generateStream(formattedPrompt, temperature, maxTokens, resetContext, stop,
                 [&result](const std::string& token, bool done) {
                   if (!done)
                     result.append(token);
                 });
  return result;
}

std::string LlmService::chat(const ChatRequest& req)
{
  std::string prompt = buildPrompt(req.messages);
  const int32_t maxTokens =
      req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
  const float temp =
      req.temperature >= 0.0F ? req.temperature : defaultTemperature_;
  return generate(prompt, temp, maxTokens, req.resetContext, req.stop);
}

void LlmService::chatStream(const ChatRequest& req, TokenCallback onToken)
{
  std::string prompt = buildPrompt(req.messages);
  const int32_t maxTokens =
      req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
  const float temp =
      req.temperature >= 0.0F ? req.temperature : defaultTemperature_;
  generateStream(prompt, temp, maxTokens, req.resetContext, req.stop,
                 std::move(onToken));
}

drogon::Task<std::string> LlmService::chatAsync(const ChatRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [this, req]() { return chat(req); });
}

drogon::Task<void> LlmService::chatStreamAsync(const ChatRequest& req,
                                               TokenCallback onToken)
{
  co_await BlockingTask<void>(
      [this, req, onToken = std::move(onToken)]() mutable {
        auto wrapped = [callback = std::move(onToken)](const std::string& token,
                                                       bool done) {
          drogon::app().getLoop()->queueInLoop(
              [callback, token, done]() { callback(token, done); });
        };
        std::string prompt = buildPrompt(req.messages);
        const int32_t maxTokens =
            req.maxTokens > 0 ? req.maxTokens : defaultMaxTokens_;
        const float temp =
            req.temperature >= 0.0F ? req.temperature : defaultTemperature_;
        generateStream(prompt, temp, maxTokens, req.resetContext, req.stop,
                       std::move(wrapped));
      });
  co_return;
}
