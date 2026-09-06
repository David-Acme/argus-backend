#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/tts/onnx-utils.hxx>
#include <shared/services/tts/remote/tts-remote.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

TtsService gTts;

std::string exeDir()
{
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return ".";
  buf[n] = '\0';
  std::string path(buf);
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

struct Silence
{
  int leadSamples{0};
  int tailSamples{0};
  int longestGapSamples{0};
  int gapStart{0};
};

Silence analyse(const std::vector<float>& pcm, float threshold)
{
  Silence s;
  const int n = static_cast<int>(pcm.size());
  if (n == 0)
    return s;

  int i = 0;
  while (i < n && std::fabs(pcm[static_cast<size_t>(i)]) < threshold)
    ++i;
  s.leadSamples = i;

  int j = n - 1;
  while (j >= 0 && std::fabs(pcm[static_cast<size_t>(j)]) < threshold)
    --j;
  s.tailSamples = n - 1 - j;

  int runStart = -1;
  for (int k = s.leadSamples; k <= j; ++k) {
    const bool quiet = std::fabs(pcm[static_cast<size_t>(k)]) < threshold;
    if (quiet && runStart < 0)
      runStart = k;
    if (!quiet && runStart >= 0) {
      const int len = k - runStart;
      if (len > s.longestGapSamples) {
        s.longestGapSamples = len;
        s.gapStart = runStart;
      }
      runStart = -1;
    }
  }
  return s;
}

void report(const char* label, const std::vector<float>& pcm, int sr)
{
  const auto s = analyse(pcm, 1e-3F);
  const double dur = static_cast<double>(pcm.size()) / sr;
  std::printf("  %-34s len=%6.2fs  lead=%5.0fms  tail=%5.0fms  "
              "longest_inner_gap=%5.0fms @%.2fs\n",
              label, dur, 1000.0 * s.leadSamples / sr,
              1000.0 * s.tailSamples / sr, 1000.0 * s.longestGapSamples / sr,
              static_cast<double>(s.gapStart) / sr);
}

} // namespace

int gFailures = 0;
constexpr size_t kMinSentenceChars = 24;

std::vector<std::string> streamSplit(const std::string& text, size_t tokenSize,
                                     size_t minChars)
{
  std::vector<std::string> out;
  std::string pending;
  bool firstSent = false;
  for (size_t i = 0; i < text.size(); i += tokenSize) {
    pending += text.substr(i, tokenSize);
    for (;;) {
      const size_t cut = completeSentenceEnd(pending, firstSent ? minChars : 0);
      if (cut == 0)
        break;
      out.push_back(pending.substr(0, cut));
      pending.erase(0, cut);
      firstSent = true;
    }
  }
  if (!pending.empty())
    out.push_back(pending);
  return out;
}

void streamingSplitCheck()
{
  std::printf("=== streaming sentence split ===\n");

  struct Case
  {
    const char* label;
    const char* text;
    size_t expected;
  };

  const Case cases[] = {
      {"two full sentences",
       "El sistema ha detectado movimiento en la puerta. Todo esta en orden.",
       2},
      {"abbreviation not split",
       "El Sr. Gomez ha llegado a la puerta principal de la casa.", 1},
      {"decimal not split",
       "La persona estaba a 3.5 metros de la camara principal.", 1},
      {"short first flushes early",
       "Claro. El sistema ha detectado movimiento en la puerta principal.", 2},
      {"short second merges",
       "El sistema ha detectado movimiento. Si. Voy a revisar la camara del "
       "salon ahora mismo.",
       2},
      {"question mark splits",
       "Quieres que revise la camara del salon? Puedo hacerlo ahora mismo.", 2},
      {"three sentences",
       "He revisado la camara del salon. No hay nadie en la sala ahora. "
       "Te aviso si eso cambia.",
       3},
  };

  const std::vector<size_t> tokenSizes = {1, 3, 7, 64};

  for (const auto& c : cases) {
    const std::string original = c.text;
    std::vector<std::string> reference;
    bool lossless = true;
    bool stable = true;

    for (size_t i = 0; i < tokenSizes.size(); ++i) {
      auto parts = streamSplit(original, tokenSizes[i], kMinSentenceChars);
      std::string joined;
      for (const auto& p : parts)
        joined += p;
      if (joined != original)
        lossless = false;
      if (i == 0)
        reference = parts;
      else if (parts != reference)
        stable = false;
    }

    const bool countOk = reference.size() == c.expected;
    const bool ok = lossless && stable && countOk;
    if (!ok)
      ++gFailures;

    std::printf("  [%s] %-26s parts=%zu want=%zu lossless=%d stable=%d\n",
                ok ? "ok" : "FAIL", c.label, reference.size(), c.expected,
                lossless ? 1 : 0, stable ? 1 : 0);
    if (!ok)
      for (const auto& p : reference)
        std::printf("         -> \"%s\"\n", p.c_str());
  }
  std::printf("\n");
}

int main(int argc, char** argv)
{
  streamingSplitCheck();

  // --http <url> probes a running argus-tts over the internal wire instead
  // of the in-process engine (no local models needed).
  std::string httpUrl;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--http" && i + 1 < argc)
      httpUrl = argv[++i];

  ConfigService::load("config.toml");

  std::function<std::vector<float>(const TtsRequest&)> synth;
  int sr = 0;
  const TtsClient remote;
  if (!httpUrl.empty()) {
    ConfigService::setRuntimeString("tts.remote_url", httpUrl);
    sr = remote.sampleRate();
    synth = [&remote](const TtsRequest& req) { return remote.synthesize(req); };
    std::printf("argus-tts wire: %s\n", httpUrl.c_str());
  }
  else {
    gTts.init();
    if (!gTts.isLoaded()) {
      std::printf("TTS NOT LOADED\n");
      return 1;
    }
    sr = gTts.sampleRate();
    synth = [](const TtsRequest& req) { return gTts.synthesize(req); };
  }
  std::printf("sample_rate=%d\n\n", sr);

  struct Case
  {
    const char* label;
    const char* text;
  };
  const Case cases[] = {
      {"one short sentence", "Hola, buenos dias."},
      {"two sentences", "Hola, buenos dias. Como estas hoy?"},
      {"three sentences", "Hola. Que tal. Todo bien."},
      {"no final period", "Hola, buenos dias"},
      {"long single sentence",
       "El sistema de seguridad ha detectado movimiento en la puerta "
       "principal y ha identificado a una persona conocida sin generar "
       "ninguna alerta relevante"},
      {"long, multi-sentence (chunked)",
       "El sistema de seguridad ha detectado movimiento en la puerta "
       "principal. Ha identificado a una persona conocida. No se ha generado "
       "ninguna alerta relevante para el usuario. La camara del salon sigue "
       "monitorizando la escena con normalidad. Todo se encuentra en orden en "
       "este momento del dia. El registro queda guardado en la base de datos "
       "local para futuras consultas del propietario de la vivienda."},
  };

  for (const auto& c : cases) {
    TtsRequest req;
    req.text = c.text;
    const auto t0 = std::chrono::steady_clock::now();
    auto pcm = synth(req);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    report(c.label, pcm, sr);
    std::printf("  %-34s synth=%.0f ms  chars=%zu\n", "", ms,
                std::string(c.text).size());
    TtsService::writeWav(exeDir() + "/probe-" + std::to_string(&c - cases) +
                             ".wav",
                         pcm, sr);
  }

  if (gFailures > 0) {
    std::printf("\n%d SPLIT FAILURE(S)\n", gFailures);
    return 1;
  }
  std::printf("\nsplit checks: ALL PASS\n");
  return 0;
}
