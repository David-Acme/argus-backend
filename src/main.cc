#include <config/application.hxx>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <llama.h>
#include <sstream>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/services/vision/vision-service.hxx>
#include <string>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

long vmRssKb()
{
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream ss(line.substr(6));
      long kb = 0;
      ss >> kb;
      return kb;
    }
  }
  return -1;
}

double msSince(Clock::time_point t0)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                               t0)
             .count() /
         1000.0;
}

void benchLlm(int runs)
{
  std::cout << "\n== LLM (LFM2.5-1.2B, Q4_K_M) ==\n";
  std::string body;
  for (int i = 0; i < 6; ++i)
    body += "Argus es un sistema de seguridad local que procesa todo en el "
            "equipo sin usar la nube. Detecta personas, vehículos y objetos "
            "anómalos usando visión por computadora, y responde preguntas "
            "sobre eventos recientes con un asistente integrado. ";
  ChatRequest req;
  req.messages.push_back({"user", body});
  req.maxTokens = 96;
  req.temperature = 0.3f;

  for (int r = 0; r < runs; ++r) {
    req.resetContext = true;
    auto t0 = Clock::now();
    auto out = LlmService::chat(req);
    std::cout << "  run " << r + 1 << ": total=" << msSince(t0)
              << "ms out_chars=" << out.size() << "\n";
  }
}

void benchVision(int runs)
{
  std::cout << "\n== VL (LFM2.5-VL-450M, text-only path) ==\n";
  VisionRequest req;
  req.imageRgb.assign(16 * 16 * 3, 0);
  req.width = 16;
  req.height = 16;
  req.maxTokens = 64;

  for (int r = 0; r < runs; ++r) {
    auto t0 = Clock::now();
    auto out = VisionService::describe(req);
    std::cout << "  run " << r + 1 << ": total=" << msSince(t0)
              << "ms out_chars=" << out.size() << "\n";
  }
}

void benchStt(int runs)
{
  std::cout << "\n== STT (Whisper tiny int8, es) ==\n";
  constexpr int kRate = 16000;
  constexpr double kSeconds = 5.0;
  std::vector<float> audio(static_cast<size_t>(kRate * kSeconds));
  for (size_t i = 0; i < audio.size(); ++i) {
    double t = static_cast<double>(i) / kRate;
    audio[i] = 0.2F * static_cast<float>(
                          std::sin(2.0 * M_PI * (300.0 + 700.0 * t) * t));
  }

  for (int r = 0; r < runs; ++r) {
    auto t0 = Clock::now();
    auto text = SttService::transcribe(audio, kRate);
    double el = msSince(t0);
    std::cout << "  run " << r + 1 << ": total=" << el << "ms (audio="
              << kSeconds << "s, RTF=" << el / 1000.0 / kSeconds
              << ", text='" << text.substr(0, 40) << "')\n";
  }
}

void benchTts(int runs)
{
  std::cout << "\n== TTS (Supertonic 3, quality=Low) ==\n";
  TtsRequest req;
  req.text =
      "Hola, esta es una prueba de síntesis de voz para medir el rendimiento "
      "del sistema en este equipo.";
  req.quality = TtsQuality::Low;

  for (int r = 0; r < runs; ++r) {
    auto t0 = Clock::now();
    auto wav = TtsService::synthesize(req);
    std::cout << "  run " << r + 1 << ": total=" << msSince(t0)
              << "ms wav_samples=" << wav.size() << "\n";
  }
}

int runBench()
{
  std::cout << "=== Argus AI benchmark ===\n";
  long rss0 = vmRssKb();
  std::cout << "base RSS: " << rss0 << " KB\n";

  llama_backend_init();

  auto t0 = Clock::now();
  LlmService::init();
  std::cout << "LLM init: " << msSince(t0) << "ms | RSS: " << vmRssKb()
            << " KB (+" << vmRssKb() - rss0 << ")\n";

  t0 = Clock::now();
  VisionService::init();
  std::cout << "Vision init: " << msSince(t0) << "ms | RSS: " << vmRssKb()
            << " KB (+" << vmRssKb() - rss0 << ")\n";

  t0 = Clock::now();
  SttService::init();
  std::cout << "STT init: " << msSince(t0) << "ms | RSS: " << vmRssKb()
            << " KB (+" << vmRssKb() - rss0 << ")\n";

  t0 = Clock::now();
  TtsService::init();
  std::cout << "TTS init: " << msSince(t0) << "ms | RSS: " << vmRssKb()
            << " KB (+" << vmRssKb() - rss0 << ")\n";

  const int runs = 3;
  benchLlm(runs);
  benchVision(runs);
  benchStt(runs);
  benchTts(runs);

  std::cout << "\npeak RSS: " << vmRssKb() << " KB (+" << vmRssKb() - rss0
            << ")\n";

  TtsService::shutdown();
  SttService::shutdown();
  VisionService::shutdown();
  LlmService::shutdown();
  llama_backend_free();
  return 0;
}

} // namespace

int main(int argc, char* argv[])
{
  if (argc > 1 && std::string(argv[1]) == "--bench")
    return runBench();

  Application app;
  return app.run();
}
