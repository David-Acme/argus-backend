#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

double msSince(Clock::time_point t0)
{
  return Ms(Clock::now() - t0).count();
}

int64_t rssKb()
{
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmRSS:", 0) == 0)
      return std::strtoll(line.c_str() + 6, nullptr, 10);
  return 0;
}

int64_t peakRssKb()
{
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmHWM:", 0) == 0)
      return std::strtoll(line.c_str() + 6, nullptr, 10);
  return 0;
}

std::vector<unsigned char> syntheticImage(int w, int h)
{
  cv::Mat img(h, w, CV_8UC3, cv::Scalar(30, 40, 55));
  for (int y = 0; y < h; ++y) {
    auto* row = img.ptr<unsigned char>(y);
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = static_cast<unsigned char>((x * 255) / std::max(1, w));
      row[x * 3 + 1] = static_cast<unsigned char>((y * 255) / std::max(1, h));
      row[x * 3 + 2] =
          static_cast<unsigned char>(((x + y) * 127) / std::max(1, w + h));
    }
  }
  cv::rectangle(img, cv::Rect(w / 6, h / 5, w / 4, h / 2),
                cv::Scalar(240, 240, 240), -1);
  cv::circle(img, cv::Point(w * 2 / 3, h / 2), std::min(w, h) / 7,
             cv::Scalar(60, 90, 220), -1);
  cv::putText(img, "ARGUS", cv::Point(w / 8, h * 9 / 10),
              cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(255, 255, 255), 3);

  std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
  cv::Mat dst(h, w, CV_8UC3, rgb.data());
  cv::cvtColor(img, dst, cv::COLOR_BGR2RGB);
  return rgb;
}

struct Result
{
  double totalMs{0.0};
  double prefillMs{0.0};
  size_t promptTokens{0};
  int genTokens{0};
  std::string text;
};

int gImageMaxTokens = 0;
bool gDebugPrompt = true;
llama_model* gModel = nullptr;
llama_context* gCtx = nullptr;
mtmd_context* gMtmd = nullptr;

Result caption(const std::vector<unsigned char>& rgb, int w, int h,
               const std::string& instruction, int maxTokens)
{
  Result r;
  const auto t0 = Clock::now();

  auto mem = llama_get_memory(gCtx);
  if (mem)
    llama_memory_clear(mem, true);

  const std::string marker = mtmd_default_marker();
  const std::string userContent = marker + "\n" + instruction;

  std::string prompt = "<|im_start|>user\n" + userContent +
                      "<|im_end|>\n<|im_start|>assistant\n";
  if (gDebugPrompt) {
    std::printf("  [prompt=%s]\n", prompt.c_str());
    gDebugPrompt = false;
  }

  mtmd_bitmap* bmp = mtmd_bitmap_init(static_cast<uint32_t>(w),
                                      static_cast<uint32_t>(h), rgb.data());
  mtmd_input_chunks* chunks = mtmd_input_chunks_init();
  mtmd_input_text text{};
  text.text = prompt.c_str();
  text.text_len = prompt.size();
  text.add_special = true;
  text.parse_special = true;

  const mtmd_bitmap* bitmaps[1] = {bmp};
  const int32_t tok = mtmd_tokenize(gMtmd, chunks, &text, bitmaps, 1);
  if (tok != 0) {
    std::printf("  mtmd_tokenize failed: %d\n", tok);
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bmp);
    return r;
  }

  r.promptTokens = mtmd_helper_get_n_tokens(chunks);

  llama_pos nPast = 0;
  const auto tPre = Clock::now();
  const int32_t ev = mtmd_helper_eval_chunks(gMtmd, gCtx, chunks, 0, 0, 512,
                                             true, &nPast);
  r.prefillMs = msSince(tPre);
  mtmd_input_chunks_free(chunks);
  mtmd_bitmap_free(bmp);
  if (ev != 0) {
    std::printf("  mtmd_helper_eval_chunks failed: %d\n", ev);
    return r;
  }

  auto sparams = llama_sampler_chain_default_params();
  sparams.no_perf = true;
  auto* smpl = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

  const auto* vocab = llama_model_get_vocab(gModel);
  const llama_token eos = llama_vocab_eos(vocab);
  const llama_token eot = llama_vocab_eot(vocab);

  auto batch = llama_batch_init(1, 0, 1);
  for (int i = 0; i < maxTokens; ++i) {
    const llama_token t = llama_sampler_sample(smpl, gCtx, -1);
    if (t == eos || t == eot)
      break;
    char piece[256];
    const int pn =
        llama_token_to_piece(vocab, t, piece, sizeof(piece), 0, true);
    if (pn > 0)
      r.text.append(piece, static_cast<size_t>(pn));
    ++r.genTokens;
    llama_sampler_accept(smpl, t);

    batch.token[0] = t;
    batch.pos[0] = nPast++;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = 1;
    batch.n_tokens = 1;
    if (llama_decode(gCtx, batch) != 0)
      break;
  }
  llama_batch_free(batch);
  llama_sampler_free(smpl);

  r.totalMs = msSince(t0);
  return r;
}

void report(const char* label, const Result& r)
{
  const double genMs = r.totalMs - r.prefillMs;
  const double tps = r.genTokens > 0 ? r.genTokens * 1000.0 / std::max(1.0, genMs) : 0.0;
  std::printf("  %-18s total=%7.1f ms  prefill=%7.1f ms  gen=%6.1f ms  "
              "prompt_tok=%4zu  gen_tok=%3d  %.1f tok/s\n",
              label, r.totalMs, r.prefillMs, genMs, r.promptTokens,
              r.genTokens, tps);
}

} // namespace

int main(int argc, char** argv)
{
  std::string modelPath = "models/vision/lfm2vl/LFM2.5-VL-450M-Q8_0.gguf";
  std::string mmprojPath = "models/vision/lfm2vl/mmproj-F16.gguf";
  int maxTokens = 48;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc)
      modelPath = argv[++i];
    else if (std::strcmp(argv[i], "--mmproj") == 0 && i + 1 < argc)
      mmprojPath = argv[++i];
    else if (std::strcmp(argv[i], "--tokens") == 0 && i + 1 < argc)
      maxTokens = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--image-max-tokens") == 0 && i + 1 < argc)
      gImageMaxTokens = std::atoi(argv[++i]);
  }

  llama_log_set(
      [](enum ggml_log_level level, const char* t, void*) {
        if (level == GGML_LOG_LEVEL_ERROR)
          fputs(t, stderr);
      },
      nullptr);

  llama_backend_init();
  const int64_t rss0 = rssKb();

  auto t0 = Clock::now();
  llama_model_params mp = llama_model_default_params();
  mp.n_gpu_layers = 0;
  mp.load_mode = LLAMA_LOAD_MODE_MMAP;
  gModel = llama_model_load_from_file(modelPath.c_str(), mp);
  if (!gModel) {
    std::printf("failed to load %s\n", modelPath.c_str());
    return 1;
  }

  llama_context_params cp = llama_context_default_params();
  cp.n_ctx = 8192;
  cp.n_batch = 512;
  cp.n_ubatch = 512;
  cp.n_threads = 8;
  cp.n_threads_batch = 8;
  cp.no_perf = true;
  gCtx = llama_init_from_model(gModel, cp);
  if (!gCtx) {
    std::printf("failed to create context\n");
    return 1;
  }
  const double lmMs = msSince(t0);
  const int64_t rssLm = rssKb();

  t0 = Clock::now();
  auto mparams = mtmd_context_params_default();
  mparams.use_gpu = false;
  mparams.print_timings = false;
  mparams.n_threads = 8;
  if (gImageMaxTokens > 0)
    mparams.image_max_tokens = gImageMaxTokens;
  gMtmd = mtmd_init_from_file(mmprojPath.c_str(), gModel, mparams);
  if (!gMtmd) {
    std::printf("failed to load mmproj %s\n", mmprojPath.c_str());
    return 1;
  }
  const double projMs = msSince(t0);
  const int64_t rssAll = rssKb();

  std::printf("model: %s\n", modelPath.c_str());
  std::printf("mmproj: %s\n", mmprojPath.c_str());
  std::printf("init: lm=%.0f ms (+%lld KB)  mmproj=%.0f ms (+%lld KB)\n", lmMs,
              static_cast<long long>(rssLm - rss0), projMs,
              static_cast<long long>(rssAll - rssLm));
  std::printf("vision=%s  audio=%s  mrope=%s  image_max_tokens=%d\n",
              mtmd_support_vision(gMtmd) ? "yes" : "no",
              mtmd_support_audio(gMtmd) ? "yes" : "no",
              mtmd_decode_use_mrope(gMtmd) ? "yes" : "no", gImageMaxTokens);

  const std::string instruction = "Can you describe this image?";

  {
    auto rgb = syntheticImage(640, 480);
    auto warm = caption(rgb, 640, 480, instruction, 8);
    (void)warm;
  }

  std::printf("\n=== latency ===\n");
  struct Case
  {
    const char* name;
    int w;
    int h;
  };
  const Case cases[] = {
      {"256x256", 256, 256},
      {"384x384", 384, 384},
      {"512x512", 512, 512},
      {"1280x720", 1280, 720},
      {"2688x1520", 2688, 1520},
  };

  for (const auto& c : cases) {
    auto rgb = syntheticImage(c.w, c.h);
    Result best;
    for (int rep = 0; rep < 3; ++rep) {
      auto r = caption(rgb, c.w, c.h, instruction, maxTokens);
      if (best.totalMs == 0.0 || r.totalMs < best.totalMs)
        best = r;
    }
    report(c.name, best);
  }

  std::printf("\n=== captions (1280x720) ===\n");
  {
    auto rgb = syntheticImage(1280, 720);
    auto r = caption(rgb, 1280, 720, instruction, 64);
    std::printf("  generic : \"%s\"\n", r.text.c_str());
    auto r2 = caption(rgb, 1280, 720,
                      "Describe what you see, including any text.", 64);
    std::printf("  w/ text : \"%s\"\n", r2.text.c_str());
    auto r3 = caption(rgb, 1280, 720,
                      "Is there a person in this image? Answer yes or no.", 16);
    std::printf("  person? : \"%s\"\n", r3.text.c_str());
  }

  std::printf("\npeak rss: %lld KB (%.2f GB)\n",
              static_cast<long long>(peakRssKb()),
              static_cast<double>(peakRssKb()) / 1048576.0);

  mtmd_free(gMtmd);
  llama_free(gCtx);
  llama_model_free(gModel);
  llama_backend_free();
  return 0;
}
