#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/vision/vision-service.hxx>
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

int64_t peakRssKb()
{
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      return std::strtoll(line.c_str() + 6, nullptr, 10);
    }
  }
  return 0;
}

int64_t currentRssKb()
{
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      return std::strtoll(line.c_str() + 6, nullptr, 10);
    }
  }
  return 0;
}

std::string repeatFiller(size_t approxChars)
{
  static const std::string unit =
      "La camara del salon registro movimiento a las tres de la tarde y el "
      "sistema identifico a una persona conocida que entro por la puerta "
      "principal sin generar ninguna alerta relevante para el usuario. ";
  std::string out;
  out.reserve(approxChars + unit.size());
  while (out.size() < approxChars)
    out += unit;
  return out;
}

struct GenStats
{
  double ttftMs{0.0};
  double totalMs{0.0};
  int tokens{0};
  std::string text;
  std::vector<double> tokenMs;
};

GenStats runLlm(const std::string& systemPrompt, const std::string& userText,
                int maxTokens)
{
  GenStats st;
  ChatRequest req;
  if (!systemPrompt.empty())
    req.messages.push_back({"system", systemPrompt});
  req.messages.push_back({"user", userText});
  req.maxTokens = maxTokens;
  req.resetContext = true;

  auto t0 = Clock::now();
  auto tPrev = t0;
  bool first = true;

  LlmService::chatStream(req, [&](const std::string& tok, bool done) {
    if (done)
      return;
    const auto now = Clock::now();
    if (first) {
      st.ttftMs = Ms(now - t0).count();
      first = false;
    }
    else {
      st.tokenMs.push_back(Ms(now - tPrev).count());
    }
    tPrev = now;
    st.text += tok;
    ++st.tokens;
  });

  st.totalMs = msSince(t0);
  return st;
}

double median(std::vector<double> v)
{
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double slopePerToken(const std::vector<double>& v)
{
  const size_t n = v.size();
  if (n < 8)
    return 0.0;
  const size_t q = n / 4;
  const double firstQ =
      std::accumulate(v.begin(), v.begin() + static_cast<long>(q), 0.0) /
      static_cast<double>(q);
  const double lastQ =
      std::accumulate(v.end() - static_cast<long>(q), v.end(), 0.0) /
      static_cast<double>(q);
  return lastQ - firstQ;
}

std::vector<unsigned char> syntheticImage(int w, int h)
{
  cv::Mat img(h, w, CV_8UC3, cv::Scalar(30, 40, 55));
  for (int y = 0; y < h; ++y) {
    auto* row = img.ptr<unsigned char>(y);
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = static_cast<unsigned char>((x * 255) / std::max(1, w));
      row[x * 3 + 1] = static_cast<unsigned char>((y * 255) / std::max(1, h));
      row[x * 3 + 2] = static_cast<unsigned char>(((x + y) * 127) /
                                                 std::max(1, w + h));
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

void benchLlm()
{
  std::printf("\n=== LLM ===\n");
  if (!LlmService::isLoaded()) {
    std::printf("  NOT LOADED\n");
    return;
  }

  const std::string sys =
      "Eres un asistente de seguridad domestica. Responde en 2 lineas.";

  {
    auto warm = runLlm(sys, "Hola.", 16);
    (void)warm;
  }

  struct Case
  {
    const char* name;
    size_t chars;
  };
  const Case cases[] = {
      {"short   (~40 tok)", 120},
      {"medium  (~350 tok)", 1200},
      {"long    (~1200 tok)", 4200},
      {"xlong   (~2900 tok)", 10000},
      {"xxlong  (~5800 tok)", 20000},
  };

  std::printf("  %-22s %10s %10s %10s %8s %7s %s\n", "prompt", "ttft_ms",
              "total_ms", "tok/s", "tokens", "drift", "ok");
  for (const auto& c : cases) {
    const std::string user =
        repeatFiller(c.chars) + "\nResume en una frase que ha pasado.";
    auto st = runLlm(sys, user, 48);
    const double tps =
        st.tokens > 1 ? (st.tokens - 1) * 1000.0 /
                            std::max(1.0, st.totalMs - st.ttftMs)
                      : 0.0;
    std::printf("  %-22s %10.1f %10.1f %10.2f %8d %7.3f %s\n", c.name,
                st.ttftMs, st.totalMs, tps, st.tokens,
                slopePerToken(st.tokenMs), st.tokens > 0 ? "yes" : "NO");
  }

  {
    auto st = runLlm(sys, "Di exactamente: prueba de determinismo.", 24);
    auto st2 = runLlm(sys, "Di exactamente: prueba de determinismo.", 24);
    std::printf("  determinism: %s\n",
                st.text == st2.text ? "identical" : "varies");
    std::printf("  sample: \"%s\"\n", st.text.substr(0, 140).c_str());
  }

  {
    const std::string user =
        "Enumera brevemente tres cosas que una camara de seguridad puede "
        "detectar en una casa.";
    auto st = runLlm(sys, user, 128);
    std::printf("  quality sample (128 tok): \"%s\"\n",
                st.text.substr(0, 400).c_str());
  }
}

void benchVlm()
{
  std::printf("\n=== VLM ===\n");
  if (!VisionService::isLoaded()) {
    std::printf("  NOT LOADED\n");
    return;
  }

  VisionRequest req;
  req.width = 1280;
  req.height = 720;
  req.imageRgb = syntheticImage(1280, 720);
  req.maxTokens = 48;

  {
    VisionRequest warm = req;
    warm.maxTokens = 8;
    auto s = VisionService::describe(warm);
    (void)s;
  }

  const int64_t rssBefore = currentRssKb();
  std::vector<double> runs;
  std::string caption;
  for (int i = 0; i < 3; ++i) {
    auto t0 = Clock::now();
    caption = VisionService::describe(req);
    runs.push_back(msSince(t0));
  }
  const int64_t rssAfter = currentRssKb();

  std::printf("  cold-ish runs (ms): ");
  for (double r : runs)
    std::printf("%.1f ", r);
  std::printf("\n  median: %.1f ms\n", median(runs));

  VisionRequest big = req;
  big.width = 2688;
  big.height = 1520;
  big.imageRgb = syntheticImage(2688, 1520);
  big.maxTokens = 48;
  auto t0 = Clock::now();
  auto capBig = VisionService::describe(big);
  const double bigMs = msSince(t0);
  std::printf("  2688x1520 input: %.1f ms\n", bigMs);

  VisionRequest tokens16 = req;
  tokens16.maxTokens = 16;
  t0 = Clock::now();
  VisionService::describe(tokens16);
  const double ms16 = msSince(t0);

  VisionRequest tokens48 = req;
  tokens48.maxTokens = 48;
  t0 = Clock::now();
  VisionService::describe(tokens48);
  const double ms48 = msSince(t0);

  const double perTokenLate = (ms48 - ms16) / 32.0;
  std::printf("  16 tok: %.1f ms | 48 tok: %.1f ms | marginal per token: "
              "%.2f ms\n",
              ms16, ms48, perTokenLate);
  std::printf("  rss delta across 3 runs: %lld KB\n",
              static_cast<long long>(rssAfter - rssBefore));
  std::printf("  caption: \"%s\"\n", caption.c_str());
  std::printf("  caption(2688): \"%s\"\n", capBig.c_str());
}

} // namespace

int main(int argc, char** argv)
{
  bool doLlm = false;
  bool doVlm = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--llm") == 0)
      doLlm = true;
    else if (std::strcmp(argv[i], "--vlm") == 0)
      doVlm = true;
    else if (std::strcmp(argv[i], "--all") == 0)
      doLlm = doVlm = true;
  }
  if (!doLlm && !doVlm)
    doLlm = doVlm = true;

  ConfigService::load("config.toml");

  const int64_t rss0 = currentRssKb();

  if (doLlm) {
    auto t0 = Clock::now();
    LlmService::init();
    std::printf("llm init: %.0f ms (rss +%lld KB)\n", msSince(t0),
                static_cast<long long>(currentRssKb() - rss0));
  }
  const int64_t rss1 = currentRssKb();
  if (doVlm) {
    auto t0 = Clock::now();
    VisionService::init();
    std::printf("vlm init: %.0f ms (rss +%lld KB)\n", msSince(t0),
                static_cast<long long>(currentRssKb() - rss1));
  }

  if (doLlm)
    benchLlm();
  if (doVlm)
    benchVlm();

  std::printf("\npeak rss: %lld KB (%.2f GB)\n",
              static_cast<long long>(peakRssKb()),
              static_cast<double>(peakRssKb()) / 1048576.0);

  if (doVlm)
    VisionService::shutdown();
  if (doLlm)
    LlmService::shutdown();
  return 0;
}
