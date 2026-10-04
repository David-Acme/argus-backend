#include <llm/llm-service.hxx>
#include "sampling-config.hxx"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <drogon/drogon.h>
#include <limits>
#include <ranges>
#include <llama.h>
#include <config/config-service.hxx>
#include <runtime/ai-init.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/thread-budget.hxx>
#include <thread>
#include <vector>

namespace
{

constexpr std::size_t kMaxCheckpoints = 8;
constexpr std::size_t kCheckpointsPerPrefill = 3;

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

}

LlmService::LlmService()
    : model_(nullptr, llama_model_free), context_(nullptr, llama_free)
{
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
    std::scoped_lock lock(ai_init::llamaMutex());

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
    tailLocked_ = llama_model_is_recurrent(model_.get()) ||
                  llama_model_is_hybrid(model_.get());
    messageStart_ = specialToken("<|im_start|>");
    toolCallStart_ = specialToken("<|tool_call_start|>");
    endTokens_.clear();
    const auto* vocab = llama_model_get_vocab(model_.get());
    for (int32_t token = 0; token < llama_vocab_n_tokens(vocab); ++token) {
      if (llama_vocab_is_eog(vocab, token))
        endTokens_.push_back(token);
    }
    promptBatch_ =
        std::make_unique<llama_batch>(llama_batch_init(nBatch_, 0, 1));
    genBatch_ = std::make_unique<llama_batch>(llama_batch_init(1, 0, 1));

    if (const char* tmpl = llama_model_chat_template(model_.get(), nullptr))
      chatTemplate_ = tmpl;
    const std::string tmplMode = ConfigService::getString("llm.chat_template");
    if (tmplMode == "chatml")
      chatTemplate_.clear();

    contextSize_ = contextSize;
    refreshSampling();

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
  std::scoped_lock lock(mutex_);
  if (promptBatch_) {
    llama_batch_free(*promptBatch_);
    promptBatch_.reset();
  }
  if (genBatch_) {
    llama_batch_free(*genBatch_);
    genBatch_.reset();
  }
  cachedTokens_.clear();
  checkpoints_.clear();
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
  return lastStats_.load();
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

int32_t LlmService::specialToken(const std::string& text)
{
  const auto tokens = tokenize(text, false);
  return tokens.size() == 1 ? tokens.front() : -1;
}

void LlmService::forgetCache()
{
  cachedTokens_.clear();
  checkpoints_.clear();
  if (auto mem = llama_get_memory(context_.get()))
    llama_memory_clear(mem, true);
}

std::size_t LlmService::rewind(std::size_t target)
{
  auto* ctx = context_.get();
  auto mem = llama_get_memory(ctx);
  if (target == 0 || !mem)
    return 0;
  if (target == cachedTokens_.size())
    return target;
  const auto keepUpTo = [this](std::size_t tokens) {
    std::erase_if(checkpoints_, [tokens](const PrefixCheckpoint& checkpoint) {
      return checkpoint.tokens > tokens;
    });
    cachedTokens_.resize(tokens);
    return tokens;
  };
  if (llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(target), -1))
    return keepUpTo(target);
  const auto newest = std::views::reverse(checkpoints_);
  const auto usable = std::ranges::find_if(
      newest, [target](const PrefixCheckpoint& checkpoint) { return checkpoint.tokens <= target; });
  if (usable == newest.end())
    return 0;
  const std::size_t restored = usable->tokens;
  if (llama_state_seq_set_data_ext(ctx, usable->state.data(), usable->state.size(), 0,
                                   LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) == 0 ||
      !llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(restored), -1))
    return 0;
  return keepUpTo(restored);
}

void LlmService::checkpoint(std::size_t tokens)
{
  auto* ctx = context_.get();
  const std::size_t size =
      llama_state_seq_get_size_ext(ctx, 0, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
  if (size == 0)
    return;
  PrefixCheckpoint taken{.tokens = tokens, .state = std::vector<std::uint8_t>(size)};
  if (llama_state_seq_get_data_ext(ctx, taken.state.data(), size, 0,
                                   LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) != size)
    return;
  std::erase_if(checkpoints_, [tokens](const PrefixCheckpoint& existing) {
    return existing.tokens >= tokens;
  });
  checkpoints_.push_back(std::move(taken));
  if (checkpoints_.size() > kMaxCheckpoints)
    checkpoints_.erase(checkpoints_.begin());
}

bool LlmService::prefill(const std::vector<int32_t>& promptTokens,
                         bool forceReset)
{
  auto* ctx = context_.get();

  std::size_t reuse = 0;
  if (!forceReset && !promptTokens.empty()) {
    const auto diverged = std::ranges::mismatch(cachedTokens_, promptTokens);
    const auto common = static_cast<std::size_t>(diverged.in2 - promptTokens.begin());
    reuse = rewind(std::min(common, promptTokens.size() - 1));
  }
  if (reuse == 0)
    forgetCache();

  lastStats_.store({.promptTokens = static_cast<int32_t>(promptTokens.size()),
                    .reusedTokens = static_cast<int32_t>(reuse),
                    .decodedTokens =
                        static_cast<int32_t>(promptTokens.size() - reuse)});

  std::vector<std::size_t> cuts;
  if (tailLocked_ && messageStart_ >= 0 && !promptTokens.empty()) {
    for (std::size_t at = promptTokens.size() - 1;
         at > reuse + 1 && cuts.size() < kCheckpointsPerPrefill; --at) {
      if (promptTokens[at] == messageStart_)
        cuts.push_back(at);
    }
    std::ranges::reverse(cuts);
  }

  auto& batch = *promptBatch_;
  const std::size_t total = promptTokens.size();
  auto nextCut = cuts.begin();
  for (std::size_t start = reuse; start < total;) {
    std::size_t end = std::min(start + static_cast<std::size_t>(nBatch_), total);
    if (nextCut != cuts.end() && *nextCut < end)
      end = *nextCut;
    const bool lastChunk = end >= total;

    for (std::size_t pos = start; pos < end; ++pos) {
      const std::size_t j = pos - start;
      batch.token[j] = promptTokens[pos];
      batch.pos[j] = static_cast<int32_t>(pos);
      batch.n_seq_id[j] = 1;
      batch.seq_id[j][0] = 0;
      batch.logits[j] = (lastChunk && pos + 1 == end) ? 1 : 0;
    }
    batch.n_tokens = static_cast<int32_t>(end - start);

    if (llama_decode(ctx, batch) != 0) {
      LOG_WARN << "LLM: prompt decode failed at offset " << start;
      forgetCache();
      return false;
    }
    if (nextCut != cuts.end() && *nextCut == end) {
      checkpoint(end);
      ++nextCut;
    }
    start = end;
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

  forgetCache();
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

void LlmService::generateStream(const GenerateInput& input,
                                TokenCallback onToken)
{
  const std::string& formattedPrompt = input.formattedPrompt;
  const float temperature = input.temperature;
  const int32_t maxTokens = input.maxTokens;
  const bool resetContext = input.resetContext;
  const std::vector<std::string>& stop = input.stop;

  std::unique_lock lock(mutex_, std::defer_lock);
  if (!input.prefillOnly)
    lock.lock();
  else if (!lock.try_lock()) {
    onToken("", true);
    return;
  }
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

  const LlmSampling sampling = this->sampling();
  llama_sampler_chain_add(smpl.get(),
                          llama_sampler_init_penalties(nVocab, sampling.penaltyLastN,
                                                       sampling.penaltyRepeat,
                                                       sampling.penaltyFreq,
                                                       sampling.penaltyPresent));
  if (!input.grammar.empty()) {
    auto* grammar =
        llama_sampler_init_grammar(vocab, input.grammar.c_str(), "root");
    if (grammar != nullptr)
      llama_sampler_chain_add(smpl.get(), grammar);
    else {
      LOG_WARN << "LLM: invalid grammar supplied";
      if (input.grammarRequired) {
        onToken("", true);
        return;
      }
      LOG_WARN << "LLM: sampling unconstrained";
    }
  }
  constexpr float kNever = -std::numeric_limits<float>::infinity();
  if (!input.toolCallsAllowed && toolCallStart_ >= 0) {
    const llama_logit_bias ban{.token = toolCallStart_, .bias = kNever};
    llama_sampler_chain_add(smpl.get(),
                            llama_sampler_init_logit_bias(nVocab, 1, &ban));
  }
  const auto addTail = [&sampling, temperature](llama_sampler* chain) {
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(sampling.topK));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(sampling.topP, 1));
    if (sampling.minP > 0.0F)
      llama_sampler_chain_add(chain, llama_sampler_init_min_p(sampling.minP, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(sampling.seed));
  };
  addTail(smpl.get());

  std::unique_ptr<llama_sampler, void (*)(llama_sampler*)> opening(nullptr,
                                                                   &llama_sampler_free);
  if (!input.toolCallsAllowed) {
    std::vector<llama_logit_bias> bans;
    bans.reserve(endTokens_.size() + 1);
    for (const int32_t token : endTokens_)
      bans.push_back({.token = token, .bias = kNever});
    if (toolCallStart_ >= 0)
      bans.push_back({.token = toolCallStart_, .bias = kNever});
    opening.reset(llama_sampler_chain_init(sparams));
    llama_sampler_chain_add(opening.get(),
                            llama_sampler_init_logit_bias(nVocab, static_cast<int32_t>(bans.size()),
                                                          bans.data()));
    addTail(opening.get());
  }

  const llama_token eosToken = llama_vocab_eos(vocab);
  const llama_token eotToken = llama_vocab_eot(vocab);
  llama_pos pos = static_cast<llama_pos>(promptTokens.size());

  auto& batch = *genBatch_;
  std::string tail;
  size_t tailKeep = 0;
  for (const auto& needle : stop)
    tailKeep = std::max(tailKeep, needle.size());
  tailKeep += 256;
  for (int32_t i = 0; i < maxTokens; ++i) {
    const bool firstOfProse = i == 0 && opening;
    const llama_token newToken =
        llama_sampler_sample(firstOfProse ? opening.get() : smpl.get(), ctx, -1);
    if (firstOfProse)
      llama_sampler_accept(smpl.get(), newToken);

    if (newToken == eosToken || newToken == eotToken ||
        llama_vocab_is_eog(vocab, newToken))
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

    batch.token[0] = newToken;
    batch.pos[0] = pos++;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = 1;
    batch.n_tokens = 1;
    if (llama_decode(ctx, batch) != 0) {
      LOG_WARN << "LLM: token decode failed at position " << pos - 1;
      forgetCache();
      break;
    }
    cachedTokens_.push_back(newToken);
  }

  onToken("", true);
}

std::string LlmService::generate(const GenerateInput& input)
{
  std::string result;
  generateStream(input, [&result](const std::string& token, bool done) {
    if (!done)
      result.append(token);
  });
  return result;
}

void LlmService::refreshSampling()
{
  const LlmSampling fresh = resolveSampling();
  std::scoped_lock lock(samplingMutex_);
  sampling_ = fresh;
}

LlmSampling LlmService::sampling() const
{
  std::scoped_lock lock(samplingMutex_);
  return sampling_;
}

GenerateInput LlmService::generateInput(const ChatRequest& req)
{
  const LlmSampling current = sampling();
  return {.formattedPrompt = buildPrompt(req.messages),
          .temperature =
              req.temperature >= 0.0F ? req.temperature : current.temperature,
          .maxTokens = req.prefillOnly ? 0
                       : req.maxTokens > 0 ? req.maxTokens
                                           : current.maxTokens,
          .resetContext = req.resetContext,
          .stop = req.stop,
          .grammar = req.grammar,
          .grammarRequired = req.grammarRequired,
          .toolCallsAllowed = req.toolCallsAllowed,
          .prefillOnly = req.prefillOnly};
}

std::string LlmService::chat(const ChatRequest& req)
{
  return generate(generateInput(req));
}

void LlmService::chatStream(const ChatRequest& req, TokenCallback onToken)
{
  generateStream(generateInput(req), std::move(onToken));
}

drogon::Task<std::string> LlmService::chatAsync(const ChatRequest& req)
{
  co_return co_await BlockingTask<std::string>(
      [this, req]() { return chat(req); }, BlockingLane::Heavy);
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
        generateStream(generateInput(req), std::move(wrapped));
      },
      BlockingLane::Heavy);
  co_return;
}
