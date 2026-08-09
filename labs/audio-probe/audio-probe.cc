#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
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
  check("perdida por bloques < 1%",
        blocks.size() >= sig.size() * 2 * 99 / 100, true);

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

} // namespace

int main()
{
  resamplerCheck();
  ringCheck();
  std::printf("\n%s (%d fallos)\n", gFailures == 0 ? "TODO OK" : "HAY FALLOS",
              gFailures);
  return gFailures == 0 ? 0 : 1;
}
