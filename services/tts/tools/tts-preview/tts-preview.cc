#include <config/config-service.hxx>
#include <feature/synthesis/services/tts-service.hxx>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int kBitsPerSample = 16;
constexpr float kPeak = 32767.0F;

struct PreviewOptions
{
  std::filesystem::path config;
  std::filesystem::path models;
  std::filesystem::path out;
  std::string textEs;
  std::string textEn;
  std::string seed{"7"};
};

struct PreviewClip
{
  std::string id;
  std::string lang;
  std::string engine;
  std::string variant;
  std::string voice;
};

std::optional<PreviewOptions> parseOptions(std::span<char*> args)
{
  PreviewOptions options;
  for (std::size_t index = 1; index + 1 < args.size(); index += 2) {
    const std::string_view flag(args[index]);
    const std::string value(args[index + 1]);
    if (flag == "--config")
      options.config = value;
    else if (flag == "--models")
      options.models = value;
    else if (flag == "--out")
      options.out = value;
    else if (flag == "--text-es")
      options.textEs = value;
    else if (flag == "--text-en")
      options.textEn = value;
    else if (flag == "--seed")
      options.seed = value;
    else
      return std::nullopt;
  }
  if (options.config.empty() || options.models.empty() || options.out.empty() || options.textEs.empty() ||
      options.textEn.empty())
    return std::nullopt;
  return options;
}

std::optional<PreviewClip> parseClip(const std::string& line)
{
  std::istringstream fields(line);
  PreviewClip clip;
  if (!(fields >> clip.id >> clip.lang >> clip.engine >> clip.variant >> clip.voice))
    return std::nullopt;
  if ((clip.lang != "es" && clip.lang != "en") || (clip.engine != "pocket" && clip.engine != "supertonic"))
    return std::nullopt;
  return clip;
}

template <typename Value>
void put(std::ofstream& out, Value value)
{
  const auto bytes = std::bit_cast<std::array<char, sizeof(Value)>>(value);
  out.write(bytes.data(), bytes.size());
}

void writeWave(const std::filesystem::path& path, std::span<const float> samples, int sampleRate)
{
  std::ofstream out(path, std::ios::binary);
  const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  out.write("RIFF", 4);
  put<std::uint32_t>(out, 36 + dataBytes);
  out.write("WAVEfmt ", 8);
  put<std::uint32_t>(out, 16);
  put<std::uint16_t>(out, 1);
  put<std::uint16_t>(out, 1);
  put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate));
  put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate) * (kBitsPerSample / 8));
  put<std::uint16_t>(out, kBitsPerSample / 8);
  put<std::uint16_t>(out, kBitsPerSample);
  out.write("data", 4);
  put<std::uint32_t>(out, dataBytes);
  for (const float sample : samples)
    put<std::int16_t>(out, static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0F, 1.0F) * kPeak)));
}

std::string pocketDirectory(const PreviewClip& clip)
{
  return clip.lang == "en" ? "en" : "es-" + clip.variant;
}

bool pocketInstalled(const PreviewOptions& options, const PreviewClip& clip)
{
  const auto directory = options.models / "pocket" / pocketDirectory(clip);
  return std::filesystem::is_regular_file(directory / "bundle.json") &&
         std::filesystem::is_regular_file(directory / "voices" / (clip.voice + ".safetensors"));
}

void select(const PreviewClip& clip)
{
  ConfigService::setRuntimeString("tts.engine_" + clip.lang, clip.engine);
  if (clip.engine == "pocket") {
    if (clip.lang == "es")
      ConfigService::setRuntimeString("tts.pocket_variant_es", clip.variant);
    ConfigService::setRuntimeString("tts.pocket_voice_" + clip.lang, clip.voice);
  }
}
}

int main(int argc, char** argv)
{
  const auto options = parseOptions(std::span(argv, static_cast<std::size_t>(argc)));
  if (!options) {
    std::cerr << "usage: argus-tts-preview --config <config.toml> --models <models/tts> --out <dir>"
                 " --text-es <text> --text-en <text> [--seed <n>] < manifest (lines: id lang engine variant voice)\n";
    return 2;
  }
  ConfigService::load(options->config.string());
  ConfigService::setRuntimeString("tts.models_dir", options->models.string());
  ConfigService::setRuntimeString("tts.pocket_seed", options->seed);
  ConfigService::setRuntimeString("tts.threads", "1");
  ConfigService::setRuntimeString("tts.normalize_text", "true");
  std::filesystem::create_directories(options->out);

  auto& service = TtsService::instance();
  service.init();
  if (!service.isLoaded()) {
    std::cerr << "Supertonic did not load from " << options->models << "\n";
    return 1;
  }
  int failures = 0;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    const auto clip = parseClip(line);
    if (!clip) {
      std::cerr << "malformed manifest line: " << line << "\n";
      ++failures;
      continue;
    }
    if (clip->engine == "pocket" && !pocketInstalled(*options, *clip)) {
      std::cerr << clip->id << ": Pocket " << pocketDirectory(*clip) << " with voice " << clip->voice
                << " is not installed\n";
      ++failures;
      continue;
    }
    select(*clip);
    const auto samples = service.synthesize({.text = clip->lang == "es" ? options->textEs : options->textEn,
                                             .lang = clip->lang == "es" ? TtsLang::ES : TtsLang::EN,
                                             .voiceId = clip->engine == "supertonic" ? clip->voice : "M3",
                                             .quality = TtsQuality::Auto,
                                             .speed = 1.0F});
    if (samples.empty()) {
      std::cerr << clip->id << ": no audio\n";
      ++failures;
      continue;
    }
    writeWave(options->out / (clip->id + ".wav"), samples, service.sampleRate());
    std::cout << clip->id << " " << static_cast<double>(samples.size()) / service.sampleRate() << " s\n";
  }
  service.shutdown();
  return failures == 0 ? 0 : 1;
}
