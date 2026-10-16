#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <shared/services/vad/vad-service.hxx>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

constexpr int kReplayWindow = 512;
constexpr std::size_t kReplayTailSamples = 16000;
constexpr const char* kBackground = "background-no-turn.wav";
const std::vector<std::string> kTurns{"turn1.wav", "turn2.wav", "turn3.wav", "turn4.wav"};

std::filesystem::path recordingsDir()
{
  if (const char* override = std::getenv("ARGUS_VOICE_REPLAY_DIR"))
    return override;
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".cache" / "argus-scratch-backup" /
         "camera-voice" / "voice-segments";
}

bool recordingsPresent()
{
  const auto dir = recordingsDir();
  if (!std::filesystem::exists(dir / kBackground))
    return false;
  for (const auto& name : kTurns)
    if (!std::filesystem::exists(dir / name))
      return false;
  return true;
}

std::vector<float> readPcm16Mono(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::size_t at = 12;
  while (at + 8 <= bytes.size()) {
    const std::string id(bytes.data() + at, 4);
    const auto size = static_cast<std::size_t>(static_cast<unsigned char>(bytes[at + 4])) |
                      (static_cast<std::size_t>(static_cast<unsigned char>(bytes[at + 5])) << 8) |
                      (static_cast<std::size_t>(static_cast<unsigned char>(bytes[at + 6])) << 16) |
                      (static_cast<std::size_t>(static_cast<unsigned char>(bytes[at + 7])) << 24);
    if (id == "data") {
      const std::size_t begin = at + 8;
      const std::size_t count = std::min(size, bytes.size() - begin) / 2;
      std::vector<float> samples(count);
      for (std::size_t i = 0; i < count; ++i) {
        const auto lo = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[begin + 2 * i]));
        const auto hi = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[begin + 2 * i + 1]));
        const auto value = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8)));
        samples[i] = static_cast<float>(value) / 32768.0F;
      }
      return samples;
    }
    at += 8 + size + (size & 1);
  }
  return {};
}

struct Replay
{
  int turns{0};
  std::vector<NearFieldVerdict> verdicts;
};

Replay replay(const std::filesystem::path& path)
{
  auto samples = readPcm16Mono(path);
  samples.insert(samples.end(), kReplayTailSamples, 0.0F);
  VadService vad(makeSileroVadModel());
  Replay out;
  NearFieldVerdict last{};
  for (std::size_t at = 0; at + kReplayWindow <= samples.size(); at += kReplayWindow) {
    const auto turn = vad.process({.samples = samples.data() + at, .count = kReplayWindow});
    if (turn)
      ++out.turns;
    const NearFieldVerdict now = vad.lastGate();
    if (now.levelDb != last.levelDb || now.heldMs != last.heldMs || now.floorDb != last.floorDb) {
      out.verdicts.push_back(now);
      last = now;
    }
  }
  return out;
}

}

struct ExpectedSegment
{
  float marginDb;
  int heldMs;
  bool passed;
};

void expectRecording(const std::string& name, int turns, const std::vector<ExpectedSegment>& segments)
{
  const Replay result = replay(recordingsDir() / name);
  CHECK(result.turns == turns);
  REQUIRE(result.verdicts.size() == segments.size());
  for (std::size_t index = 0; index < segments.size(); ++index) {
    INFO(name << " segment " << index << " margin=" << result.verdicts[index].marginDb
              << " held=" << result.verdicts[index].heldMs);
    CHECK(result.verdicts[index].marginDb == doctest::Approx(segments[index].marginDb).epsilon(0.02));
    CHECK(result.verdicts[index].heldMs == segments[index].heldMs);
    CHECK(result.verdicts[index].passed == segments[index].passed);
  }
}

TEST_CASE("the background voice that is not David opens no turn: its level margin is below the gate")
{
  if (!recordingsPresent())
    return;
  expectRecording(kBackground, 0, {{.marginDb = 27.8F, .heldMs = 992, .passed = false}});
}

TEST_CASE("turn1 opens one turn: its level margin passes the gate")
{
  if (!recordingsPresent())
    return;
  expectRecording("turn1.wav", 1, {{.marginDb = 43.0F, .heldMs = 544, .passed = true}});
}

TEST_CASE("turn2 opens one turn from its second segment; the first segment is too quiet to pass the gate")
{
  if (!recordingsPresent())
    return;
  expectRecording("turn2.wav", 1,
                  {{.marginDb = 23.9F, .heldMs = 1184, .passed = false},
                   {.marginDb = 39.7F, .heldMs = 2304, .passed = true}});
}

TEST_CASE("turn3 opens one turn: its level margin passes the gate")
{
  if (!recordingsPresent())
    return;
  expectRecording("turn3.wav", 1, {{.marginDb = 41.9F, .heldMs = 1024, .passed = true}});
}

TEST_CASE("turn4 opens one turn: its level margin passes the gate")
{
  if (!recordingsPresent())
    return;
  expectRecording("turn4.wav", 1, {{.marginDb = 39.1F, .heldMs = 1984, .passed = true}});
}

int main(int argc, char** argv)
{
  if (!recordingsPresent())
    return 77;
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
