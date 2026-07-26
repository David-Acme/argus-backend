#include "llm-service.hxx"

#include <cstring>
#include <drogon/drogon.h>
#include <llama.h>
#include <thread>
#include <vector>

std::unique_ptr<llama_model, void (*)(llama_model*)>
    LlmService::model_{nullptr, llama_model_free};
std::unique_ptr<llama_context, void (*)(llama_context*)>
    LlmService::context_{nullptr, llama_free};
int64_t LlmService::contextSize_ = 0;
bool LlmService::loaded_ = false;

void LlmService::init()
{
  try {
    llama_log_set(
        [](enum ggml_log_level level, const char* text, void*) {
          if (level == GGML_LOG_LEVEL_WARN ||
              level == GGML_LOG_LEVEL_ERROR) {
            fputs(text, stderr);
          }
        },
        nullptr);
    llama_backend_init();

    const std::string modelPath = "models/llm/LFM2.5-1.2B-Instruct-Q4_K_M.gguf";
    constexpr int64_t contextSize = 65536;

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = 0;

    llama_model* rawModel =
        llama_model_load_from_file(modelPath.c_str(), modelParams);
    if (!rawModel) {
      throw std::runtime_error("failed to load model: " + modelPath);
    }
    model_.reset(rawModel);

    auto nThreads = static_cast<int32_t>(std::thread::hardware_concurrency());

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = static_cast<uint32_t>(contextSize);
    ctxParams.n_batch = 512;
    ctxParams.n_threads = nThreads;
    ctxParams.n_threads_batch = nThreads;

    llama_context* rawCtx = llama_init_from_model(model_.get(), ctxParams);
    if (!rawCtx) {
      throw std::runtime_error("failed to create context");
    }
    context_.reset(rawCtx);

    contextSize_ = contextSize;
    loaded_ = true;

    LOG_INFO << "LLM loaded: " << modelPath << " (ctx=" << contextSize
             << ", threads=" << nThreads << ")";
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
  llama_backend_free();

  LOG_INFO << "LLM shutdown";
}

bool LlmService::isLoaded()
{
  return loaded_;
}

std::string LlmService::chat(const ChatRequest& req)
{
  auto* ctx = context_.get();
  auto* model = model_.get();
  auto* vocab = llama_model_get_vocab(model);

  int nTokens = llama_tokenize(vocab, req.prompt.c_str(),
                               static_cast<int32_t>(req.prompt.size()), nullptr,
                               0, true, true);

  if (nTokens < 0) {
    LOG_WARN << "LLM: tokenization failed";
    return "";
  }

  std::vector<llama_token> promptTokens(static_cast<size_t>(nTokens));
  llama_tokenize(vocab, req.prompt.c_str(),
                 static_cast<int32_t>(req.prompt.size()), promptTokens.data(),
                 nTokens, true, true);

  auto batch = llama_batch_get_one(promptTokens.data(), nTokens);
  if (llama_decode(ctx, batch) != 0) {
    LOG_WARN << "LLM: prompt decode failed";
    return "";
  }

  auto sparams = llama_sampler_chain_default_params();
  auto* smpl = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(smpl, llama_sampler_init_temp(req.temperature));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));

  std::string result;
  llama_token eosToken = llama_vocab_eos(vocab);

  for (int32_t i = 0; i < req.maxTokens; ++i) {
    llama_token newToken = llama_sampler_sample(smpl, ctx, -1);

    if (newToken == eosToken)
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, newToken, buf, sizeof(buf), 0, true);
    if (n > 0)
      result.append(buf, n);

    llama_sampler_accept(smpl, newToken);

    auto nextBatch = llama_batch_get_one(&newToken, 1);
    if (llama_decode(ctx, nextBatch) != 0)
      break;
  }

  llama_sampler_free(smpl);
  return result;
}
