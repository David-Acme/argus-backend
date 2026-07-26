#include "vision-service.hxx"

#include <cstring>
#include <drogon/drogon.h>
#include <llama.h>
#include <mtmd.h>
#include <thread>
#include <vector>

std::unique_ptr<llama_model, void (*)(llama_model*)>
    VisionService::model_{nullptr, llama_model_free};
std::unique_ptr<llama_context, void (*)(llama_context*)>
    VisionService::context_{nullptr, llama_free};
std::unique_ptr<mtmd_context, void (*)(mtmd_context*)>
    VisionService::mtmd_{nullptr, mtmd_free};
int64_t VisionService::contextSize_ = 0;
bool VisionService::loaded_ = false;

void VisionService::init()
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

    const std::string modelPath = "models/vision/LFM2.5-VL-450M-Q4_K_M.gguf";
    const std::string mmprojPath = "";
    constexpr int64_t contextSize = 32768;

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

    if (!mmprojPath.empty()) {
      mtmd_context_params mtmdParams = mtmd_context_params_default();
      mtmdParams.use_gpu = false;
      mtmdParams.print_timings = false;
      mtmdParams.n_threads = nThreads;
      mtmdParams.verbosity = GGML_LOG_LEVEL_ERROR;

      mtmd_context* rawMtmd =
          mtmd_init_from_file(mmprojPath.c_str(), model_.get(), mtmdParams);
      if (!rawMtmd) {
        throw std::runtime_error("failed to init mtmd: " + mmprojPath);
      }
      mtmd_.reset(rawMtmd);
    }

    contextSize_ = contextSize;
    loaded_ = true;

    LOG_INFO << "Vision loaded: " << modelPath << " (ctx=" << contextSize
             << ", threads=" << nThreads << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "Vision init failed: " << e.what();
    shutdown();
  }
}

void VisionService::shutdown()
{
  mtmd_.reset();
  context_.reset();
  model_.reset();
  loaded_ = false;
  llama_backend_free();

  LOG_INFO << "Vision shutdown";
}

bool VisionService::isLoaded()
{
  return loaded_;
}

std::string VisionService::describe(const VisionRequest& req)
{
  if (!loaded_) {
    LOG_WARN << "Vision: service not loaded";
    return "";
  }

  auto* model = model_.get();
  auto* ctx = context_.get();
  auto* vocab = llama_model_get_vocab(model);

  auto imageMarker = mtmd_default_marker();
  std::string fullPrompt = req.prompt + "\n" + std::string(imageMarker) + "\n";

  if (mtmd_) {
    auto* mctx = mtmd_.get();

    mtmd_bitmap* rawBitmap =
        mtmd_bitmap_init(req.width, req.height, req.imageRgb.data());
    if (!rawBitmap) {
      LOG_WARN << "Vision: failed to create bitmap";
      return "";
    }

    mtmd::bitmap bitmap(rawBitmap);
    mtmd::bitmaps bitmaps;
    bitmaps.entries.push_back(std::move(bitmap));
    auto bmPtrs = bitmaps.c_ptr();

    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    mtmd_input_text text{fullPrompt.c_str(), true, true};

    int32_t tokRes = mtmd_tokenize(mctx, chunks.ptr.get(), &text, bmPtrs.data(),
                                   bmPtrs.size());
    if (tokRes != 0) {
      LOG_WARN << "Vision: tokenization failed (code=" << tokRes << ")";
      return "";
    }

    for (size_t i = 0; i < chunks.size(); ++i) {
      auto* chunk = chunks[i];
      if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
        if (mtmd_encode_chunk(mctx, chunk) != 0) {
          LOG_WARN << "Vision: image encode failed";
          return "";
        }

        size_t nTokenOut = 0;
        auto* tokens = mtmd_input_chunk_get_tokens_text(chunk, &nTokenOut);
        std::vector<llama_token> tokenVec(tokens, tokens + nTokenOut);
        auto batch = llama_batch_get_one(tokenVec.data(),
                                         static_cast<int32_t>(nTokenOut));
        llama_decode(ctx, batch);
      }
      else {
        size_t nTokenOut = 0;
        auto* tokens = mtmd_input_chunk_get_tokens_text(chunk, &nTokenOut);

        std::vector<llama_token> tokenVec(tokens, tokens + nTokenOut);
        auto batch = llama_batch_get_one(tokenVec.data(),
                                         static_cast<int32_t>(nTokenOut));
        llama_decode(ctx, batch);
      }
    }
  }
  else {
    int nTokens = llama_tokenize(vocab, fullPrompt.c_str(),
                                 static_cast<int32_t>(fullPrompt.size()),
                                 nullptr, 0, true, true);
    if (nTokens < 0) {
      LOG_WARN << "Vision: tokenization failed";
      return "";
    }

    std::vector<llama_token> promptTokens(static_cast<size_t>(nTokens));
    llama_tokenize(vocab, fullPrompt.c_str(),
                   static_cast<int32_t>(fullPrompt.size()), promptTokens.data(),
                   nTokens, true, true);

    auto batch = llama_batch_get_one(promptTokens.data(), nTokens);
    if (llama_decode(ctx, batch) != 0) {
      LOG_WARN << "Vision: prompt decode failed";
      return "";
    }
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
