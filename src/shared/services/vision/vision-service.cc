#include "vision-service.hxx"

#include <cstdint>
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
bool VisionService::hasEncoder_ = false;

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
    constexpr int64_t contextSize = 32768;

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = 0;
    modelParams.use_mmap = false;
    modelParams.use_mlock = false;

    llama_model* rawModel =
        llama_model_load_from_file(modelPath.c_str(), modelParams);
    if (!rawModel) {
      throw std::runtime_error("failed to load model: " + modelPath);
    }
    model_.reset(rawModel);

    hasEncoder_ = llama_model_has_encoder(model_.get());

    auto nThreads = static_cast<int32_t>(2);

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx = static_cast<uint32_t>(contextSize);
    ctxParams.n_batch = 1024;
    ctxParams.n_ubatch = 512;
    ctxParams.n_threads = nThreads;
    ctxParams.n_threads_batch = nThreads;
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

    if (hasEncoder_) {
      mtmd_context_params mtmdParams = mtmd_context_params_default();
      mtmdParams.use_gpu = false;
      mtmdParams.print_timings = false;
      mtmdParams.n_threads = nThreads;
      mtmdParams.verbosity = GGML_LOG_LEVEL_ERROR;

      mtmd_context* rawMtmd =
          mtmd_init_from_file(modelPath.c_str(), model_.get(), mtmdParams);
      if (!rawMtmd) {
        LOG_WARN << "Vision: mtmd init failed with model file, "
                     "vision will be text-only";
        hasEncoder_ = false;
      }
      else {
        mtmd_.reset(rawMtmd);
      }
    }

    contextSize_ = contextSize;
    loaded_ = true;

    LOG_INFO << "Vision loaded: " << modelPath << " (ctx=" << contextSize
             << ", threads=" << nThreads << ", encoder="
             << (hasEncoder_ ? "yes" : "no") << ")";
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

std::string VisionService::buildVisionPrompt(
    const std::string& userPrompt)
{
  std::string prompt;
  prompt += "<|im_start|>system\n";
  prompt += SYSTEM_PROMPT;
  prompt += "<|im_end|>\n";
  prompt += "<|im_start|>user\n";
  prompt += userPrompt;
  prompt += "\n";
  prompt += mtmd_default_marker();
  prompt += "\n<|im_end|>\n";
  prompt += "<|im_start|>assistant\n";
  return prompt;
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

  auto mem = llama_get_memory(ctx);
  if (mem) {
    llama_memory_seq_rm(mem, 0, 0, -1);
  }

  std::string fullPrompt = buildVisionPrompt(req.prompt);

  if (hasEncoder_ && mtmd_) {
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

    int32_t tokRes = mtmd_tokenize(mctx, chunks.ptr.get(), &text,
                                   bmPtrs.data(), bmPtrs.size());
    if (tokRes != 0) {
      LOG_WARN << "Vision: tokenization failed (code=" << tokRes << ")";
      return "";
    }

    llama_pos currentPos = 0;

    for (size_t i = 0; i < chunks.size(); ++i) {
      auto* chunk = chunks[i];
      auto chunkType = mtmd_input_chunk_get_type(chunk);

      if (chunkType == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
        if (mtmd_encode_chunk(mctx, chunk) != 0) {
          LOG_WARN << "Vision: image encode failed";
          return "";
        }

        size_t nTokenOut = 0;
        auto* tokens = mtmd_input_chunk_get_tokens_text(chunk, &nTokenOut);
        std::vector<llama_token> tokenVec(tokens, tokens + nTokenOut);

        auto batch = llama_batch_init(static_cast<int32_t>(nTokenOut),
                                      0, 1);
        for (int32_t j = 0; j < static_cast<int32_t>(nTokenOut); ++j) {
          batch.token[j] = tokenVec[j];
          batch.pos[j] = currentPos++;
          batch.n_seq_id[j] = 1;
          batch.seq_id[j][0] = 0;
          batch.logits[j] = (j == static_cast<int32_t>(nTokenOut) - 1) ? 1 : 0;
        }
        batch.n_tokens = static_cast<int32_t>(nTokenOut);

        llama_decode(ctx, batch);
        llama_batch_free(batch);
      }
      else {
        size_t nTokenOut = 0;
        auto* tokens = mtmd_input_chunk_get_tokens_text(chunk, &nTokenOut);
        std::vector<llama_token> tokenVec(tokens, tokens + nTokenOut);

        auto batch = llama_batch_init(static_cast<int32_t>(nTokenOut),
                                      0, 1);
        for (int32_t j = 0; j < static_cast<int32_t>(nTokenOut); ++j) {
          batch.token[j] = tokenVec[j];
          batch.pos[j] = currentPos++;
          batch.n_seq_id[j] = 1;
          batch.seq_id[j][0] = 0;
          batch.logits[j] =
              (j == static_cast<int32_t>(nTokenOut) - 1) ? 1 : 0;
        }
        batch.n_tokens = static_cast<int32_t>(nTokenOut);

        llama_decode(ctx, batch);
        llama_batch_free(batch);
      }
    }
  }
  else {
    auto promptLen = static_cast<int32_t>(fullPrompt.size());
    std::vector<llama_token> promptTokens(
        static_cast<size_t>(promptLen) * 2);

    int nTokens =
        llama_tokenize(vocab, fullPrompt.c_str(), promptLen,
                       promptTokens.data(),
                       static_cast<int32_t>(promptTokens.size()),
                       true, true);

    if (nTokens < 0) {
      if (nTokens == INT32_MIN) {
        LOG_WARN << "Vision: token count overflow";
        return "";
      }
      auto needed = static_cast<size_t>(-nTokens);
      promptTokens.resize(needed);
      nTokens = llama_tokenize(vocab, fullPrompt.c_str(), promptLen,
                               promptTokens.data(),
                               static_cast<int32_t>(needed), true, true);
    }

    if (nTokens < 0) {
      LOG_WARN << "Vision: tokenization failed (code=" << nTokens << ")";
      return "";
    }

    promptTokens.resize(static_cast<size_t>(nTokens));

    auto batch = llama_batch_get_one(promptTokens.data(), nTokens);
    if (llama_decode(ctx, batch) != 0) {
      LOG_WARN << "Vision: prompt decode failed";
      return "";
    }
  }

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  auto* smpl = llama_sampler_chain_init(sparams);

  llama_sampler_chain_add(smpl, llama_sampler_init_temp(req.temperature));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
      64, 1.1f, 0.0f, 0.0f));
  llama_sampler_chain_add(smpl, llama_sampler_init_dist(42));

  std::string result;
  llama_token eosToken = llama_vocab_eos(vocab);
  llama_token eotToken = llama_vocab_eot(vocab);

  for (int32_t i = 0; i < req.maxTokens; ++i) {
    llama_token newToken = llama_sampler_sample(smpl, ctx, -1);

    if (newToken == eosToken || newToken == eotToken)
      break;

    char buf[256];
    int n = llama_token_to_piece(vocab, newToken, buf, sizeof(buf), 0, true);
    if (n > 0)
      result.append(buf, n);

    llama_sampler_accept(smpl, newToken);

    auto nextBatch = llama_batch_init(1, 0, 1);
    nextBatch.token[0] = newToken;
    nextBatch.pos[0] = i + 1;
    nextBatch.n_seq_id[0] = 1;
    nextBatch.seq_id[0][0] = 0;
    nextBatch.logits[0] = 1;
    nextBatch.n_tokens = 1;
    if (llama_decode(ctx, nextBatch) != 0) {
      llama_batch_free(nextBatch);
      break;
    }
    llama_batch_free(nextBatch);
  }

  llama_sampler_free(smpl);
  return result;
}
