#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <shared/services/vad/vad-service.hxx>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

int gFailures = 0;

void check(const char* what, bool got, bool want)
{
  const bool ok = got == want;
  if (!ok)
    gFailures++;
  std::printf("  [%s] %s (got=%d want=%d)\n", ok ? "OK" : "FAIL", what,
              static_cast<int>(got), static_cast<int>(want));
}

std::vector<int16_t> tone(int rate, double hz, double seconds)
{
  const int n = static_cast<int>(rate * seconds);
  std::vector<int16_t> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    out[static_cast<size_t>(i)] =
        static_cast<int16_t>(12000.0 * std::sin(2.0 * M_PI * hz * i / rate));
  return out;
}

std::vector<int16_t> voiceLike(int rate, double hz, double seconds)
{
  const int n = static_cast<int>(rate * seconds);
  std::vector<int16_t> out(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double env = 0.5 + 0.5 * std::sin(2.0 * M_PI * 4.0 * t);
    const double v = 0.5 * std::sin(2.0 * M_PI * hz * t) +
                     0.3 * std::sin(2.0 * M_PI * hz * 2.0 * t) +
                     0.15 * std::sin(2.0 * M_PI * hz * 3.0 * t);
    out[static_cast<size_t>(i)] = static_cast<int16_t>(12000.0 * env * v);
  }
  return out;
}

void resamplerCheck()
{
  std::printf("\n=== resampler ===\n");
  const auto sig = tone(8000, 500.0, 4.0);

  AudioResampler whole({.sourceRate = 8000, .targetRate = 16000});
  std::vector<int16_t> once;
  whole.process(sig.data(), sig.size(), once);

  AudioResampler streamed({.sourceRate = 8000, .targetRate = 16000});
  std::vector<int16_t> blocks;
  std::vector<int16_t> tmp;
  for (size_t i = 0; i + 1024 <= sig.size(); i += 1024) {
    streamed.process(sig.data() + i, 1024, tmp);
    blocks.insert(blocks.end(), tmp.begin(), tmp.end());
  }

  const size_t common = std::min(once.size(), blocks.size());
  size_t differing = 0;
  for (size_t i = 0; i < common; ++i)
    if (once[i] != blocks[i])
      differing++;

  check("streaming == una pasada", differing == 0, true);
  check("perdida por bloques < 1%", blocks.size() >= sig.size() * 2 * 99 / 100,
        true);

  AudioResampler down({.sourceRate = 44100, .targetRate = 8000});
  const auto voice = tone(44100, 440.0, 3.0);
  std::vector<int16_t> out8k;
  down.process(voice.data(), voice.size(), out8k);
  check("44.1k->8k conserva la duracion",
        out8k.size() > 23800 && out8k.size() < 24100, true);

  AudioResampler same({.sourceRate = 16000, .targetRate = 16000});
  std::vector<int16_t> passthrough;
  same.process(sig.data(), 512, passthrough);
  check("mismo ritmo es passthrough", passthrough.size() == 512, true);
}

void ringCheck()
{
  std::printf("\n=== sample ring ===\n");
  SampleRing ring(1024);
  std::vector<float> in(600, 0.5F);
  ring.push(in.data(), in.size());
  check("ring acumula", ring.size() == 600, true);

  std::vector<float> out(512);
  check("ring entrega un bloque", ring.pop(out.data(), 512), true);
  check("ring descuenta lo entregado", ring.size() == 88, true);
  check("ring no entrega de mas", ring.pop(out.data(), 512), false);

  ring.push(in.data(), in.size());
  ring.push(in.data(), in.size());
  check("ring acota la memoria", ring.size() <= 1024, true);
}

void vadCheck()
{
  std::printf("\n=== vad ===\n");
  std::vector<float> speech;
  const auto pcm = tone(16000, 220.0, 1.5);
  for (const auto s : pcm)
    speech.push_back(static_cast<float>(s) / 32768.0F);
  std::vector<float> signal(16000, 0.0F);
  signal.insert(signal.end(), speech.begin(), speech.end());
  signal.insert(signal.end(), 16000, 0.0F);

  const auto probs = [&](size_t block) {
    VadService vad;
    VadTurn turn;
    std::vector<float> out;
    for (size_t i = 0; i + block <= signal.size(); i += block) {
      vad.process(signal.data() + i, static_cast<int>(block), turn);
      out.push_back(vad.lastProb());
    }
    return out;
  };

  const auto a = probs(512);
  const auto b = probs(1920);
  size_t compared = 0;
  size_t equal = 0;
  for (size_t k = 0; k < b.size(); ++k) {
    const size_t ia = ((k + 1) * 1920) / 512 - 1;
    if (ia >= a.size())
      break;
    compared++;
    if (std::fabs(a[ia] - b[k]) < 1e-4F)
      equal++;
  }
  check("VAD independiente del tamano de bloque", equal == compared, true);
}

void vadGateCheck()
{
  std::printf("\n=== vad gate ===\n");

  std::vector<float> blip(16000, 0.0F);
  const auto pcm = tone(16000, 300.0, 0.12);
  for (size_t i = 0; i < pcm.size(); ++i)
    blip[8000 + i] = static_cast<float>(pcm[i]) / 32768.0F;
  blip.insert(blip.end(), 16000, 0.0F);

  VadService vad;
  VadTurn turn;
  bool fired = false;
  for (size_t i = 0; i + 512 <= blip.size(); i += 512)
    if (vad.process(blip.data() + i, 512, turn))
      fired = true;
  check("un blip de 120ms no genera turno", fired, false);

  std::vector<float> utterance(8000, 0.0F);
  const auto voice = voiceLike(16000, 220.0, 1.2);
  for (const auto s : voice)
    utterance.push_back(static_cast<float>(s) / 32768.0F);
  utterance.insert(utterance.end(), 16000, 0.0F);

  VadService vad2;
  VadTurn turn2;
  bool fired2 = false;
  float meanProb = 0.0F;
  for (size_t i = 0; i + 512 <= utterance.size(); i += 512) {
    if (vad2.process(utterance.data() + i, 512, turn2)) {
      fired2 = true;
      meanProb = turn2.meanProb;
    }
  }
  check("1.2s de voz si genera turno", fired2, true);
  check("el turno reporta su probabilidad media", meanProb > 0.0F, true);
}

} // namespace

int main()
{
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    const std::string path(buf);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && chdir(path.substr(0, slash).c_str()) != 0)
      std::cerr << "Warning: could not chdir\n";
  }
  resamplerCheck();
  ringCheck();
  vadCheck();
  vadGateCheck();
  std::printf("\n%s (%d fallos)\n", gFailures == 0 ? "TODO OK" : "HAY FALLOS",
              gFailures);
  return gFailures == 0 ? 0 : 1;
}
