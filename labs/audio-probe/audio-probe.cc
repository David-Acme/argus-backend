#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <shared/services/tapo/tapo-audio.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

int gFailures = 0;
int gArgc = 0;
char** gArgv = nullptr;

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

  check("streaming == single pass", differing == 0, true);
  check("block loss < 1%", blocks.size() >= sig.size() * 2 * 99 / 100, true);

  AudioResampler down({.sourceRate = 44100, .targetRate = 8000});
  const auto voice = tone(44100, 440.0, 3.0);
  std::vector<int16_t> out8k;
  down.process(voice.data(), voice.size(), out8k);
  check("44.1k->8k preserves duration",
        out8k.size() > 23800 && out8k.size() < 24100, true);

  AudioResampler same({.sourceRate = 16000, .targetRate = 16000});
  std::vector<int16_t> passthrough;
  same.process(sig.data(), 512, passthrough);
  check("same rate is passthrough", passthrough.size() == 512, true);
}

void ringCheck()
{
  std::printf("\n=== sample ring ===\n");
  SampleRing ring(1024);
  std::vector<float> in(600, 0.5F);
  ring.push(in.data(), in.size());
  check("ring accumulates", ring.size() == 600, true);

  std::vector<float> out(512);
  check("ring delivers a block", ring.pop(out.data(), 512), true);
  check("ring accounts for what was delivered", ring.size() == 88, true);
  check("ring does not overdeliver", ring.pop(out.data(), 512), false);

  ring.push(in.data(), in.size());
  ring.push(in.data(), in.size());
  check("ring bounds memory", ring.size() <= 1024, true);
}

void g711Check()
{
  std::printf("\n=== g711 ===\n");
  const auto alaw0 = tapo_audio::decodeALaw(std::vector<uint8_t>{0x55});
  const auto alawMax = tapo_audio::decodeALaw(std::vector<uint8_t>{0xAA});
  const auto alawNeg = tapo_audio::decodeALaw(std::vector<uint8_t>{0x00});
  check("alaw zero is ~0", alaw0[0] > -64 && alaw0[0] < 64, true);
  check("alaw max = 32256", alawMax[0] == 32256, true);
  check("alaw negative", alawNeg[0] < 0, true);

  const auto ulaw0 = tapo_audio::decodeULaw(std::vector<uint8_t>{0x7F});
  const auto ulawMax = tapo_audio::decodeULaw(std::vector<uint8_t>{0x80});
  check("ulaw zero is 0", ulaw0[0] == 0, true);
  check("ulaw max = 32124", ulawMax[0] == 32124, true);
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
  check("VAD independent of block size", equal == compared, true);
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
  check("a 120ms blip does not generate a turn", fired, false);

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
  check("1.2s of voice does generate a turn", fired2, true);
  check("the turn reports its mean probability", meanProb > 0.0F, true);
}

bool readWav16k(const std::string& path, std::vector<float>& out)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  std::string data((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  if (data.size() < 44 || data.compare(0, 4, "RIFF") != 0 ||
      data.compare(8, 4, "WAVE") != 0)
    return false;
  size_t cursor = 12;
  int rate = 0;
  int channels = 0;
  int bits = 0;
  size_t dataOffset = 0;
  size_t dataSize = 0;
  const auto le32 = [](const char* d) -> uint32_t {
    return static_cast<uint32_t>(static_cast<unsigned char>(d[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[3])) << 24);
  };
  const auto le16 = [](const char* d) -> uint16_t {
    return static_cast<uint16_t>(static_cast<unsigned char>(d[0]) |
                                 (static_cast<unsigned char>(d[1]) << 8));
  };
  while (cursor + 8 <= data.size()) {
    const std::string id = data.substr(cursor, 4);
    const uint32_t size = le32(data.data() + cursor + 4);
    const size_t body = cursor + 8;
    if (id == "fmt " && body + 16 <= data.size()) {
      channels = le16(data.data() + body + 2);
      rate = static_cast<int>(le32(data.data() + body + 4));
      bits = le16(data.data() + body + 14);
    }
    else if (id == "data") {
      dataOffset = body;
      dataSize = std::min(static_cast<size_t>(size), data.size() - body);
    }
    cursor = body + size + (size % 2);
  }
  if (rate != 16000 || channels != 1 || bits != 16 || dataOffset == 0)
    return false;
  out.clear();
  out.reserve(dataSize / 2);
  for (size_t i = 0; i + 1 < dataSize; i += 2) {
    const int16_t sample =
        static_cast<int16_t>(le16(data.data() + dataOffset + i));
    out.push_back(static_cast<float>(sample) / 32768.0F);
  }
  return !out.empty();
}

void vadReport(const std::string& path, const std::vector<float>& samples)
{
  VadService vad;
  VadTurn turn;
  int windows = 0;
  int turns = 0;
  float maxProb = 0.0F;
  double probSum = 0.0;
  std::vector<int> turnMs;
  for (size_t i = 0; i + 512 <= samples.size(); i += 512) {
    const bool fired = vad.process(samples.data() + i, 512, turn);
    windows++;
    maxProb = std::max(maxProb, vad.lastProb());
    probSum += vad.lastProb();
    if (fired) {
      turns++;
      turnMs.push_back(static_cast<int>(turn.samples.size() * 1000 / 16000));
    }
  }

  std::printf("%s: %.2fs windows=%d turns=%d meanProb=%.3f maxProb=%.3f\n",
              path.c_str(), samples.size() / 16000.0, windows, turns,
              probSum / std::max(1, windows), maxProb);
  for (size_t i = 0; i < turnMs.size(); ++i)
    std::printf("  turn %zu: %d ms\n", i + 1, turnMs[i]);
}

void vadProbs(const std::string& path, const std::vector<float>& samples)
{
  (void)path;
  VadService vad;
  VadTurn turn;
  for (size_t i = 0; i + 512 <= samples.size(); i += 512) {
    vad.process(samples.data() + i, 512, turn);
    std::printf("%zu %.4f\n", i * 1000 / 16000, vad.lastProb());
  }
}

} // namespace
int main(int argc, char** argv)
{
  gArgc = argc;
  gArgv = argv;
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    const std::string path(buf);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && chdir(path.substr(0, slash).c_str()) != 0)
      std::cerr << "Warning: could not chdir\n";
  }

  std::string vadWav;
  std::string probsWav;
  for (int i = 1; i < gArgc; ++i) {
    if (std::string(gArgv[i]) == "--vad" && i + 1 < gArgc)
      vadWav = gArgv[++i];
    else if (std::string(gArgv[i]) == "--vad-probs" && i + 1 < gArgc)
      probsWav = gArgv[++i];
  }
  if (!vadWav.empty()) {
    std::vector<float> samples;
    if (!readWav16k(vadWav, samples)) {
      std::printf("could not read %s\n", vadWav.c_str());
      return 1;
    }
    vadReport(vadWav, samples);
    return 0;
  }
  if (!probsWav.empty()) {
    std::vector<float> samples;
    if (!readWav16k(probsWav, samples)) {
      std::printf("could not read %s\n", probsWav.c_str());
      return 1;
    }
    vadProbs(probsWav, samples);
    return 0;
  }

  resamplerCheck();
  ringCheck();
  g711Check();
  vadCheck();
  vadGateCheck();
  std::printf("\n%s (%d failures)\n",
              gFailures == 0 ? "ALL OK" : "THERE ARE FAILURES", gFailures);
  return gFailures == 0 ? 0 : 1;
}
