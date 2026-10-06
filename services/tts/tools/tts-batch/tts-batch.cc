#include "../wave-writer.hxx"

#include <config/config-service.hxx>
#include <feature/synthesis/services/tts-service.hxx>

#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

struct BatchOptions
{
  std::filesystem::path config;
  std::filesystem::path models;
  std::filesystem::path out;
  std::string threads{"1"};
};

std::optional<BatchOptions> parseOptions(std::span<char*> args)
{
  BatchOptions options;
  for (std::size_t index = 1; index + 1 < args.size(); index += 2) {
    const std::string_view flag(args[index]);
    const std::string value(args[index + 1]);
    if (flag == "--config")
      options.config = value;
    else if (flag == "--models")
      options.models = value;
    else if (flag == "--out")
      options.out = value;
    else if (flag == "--threads")
      options.threads = value;
    else
      return std::nullopt;
  }
  if (options.config.empty() || options.models.empty() || options.out.empty())
    return std::nullopt;
  return options;
}

struct Request
{
  std::string id;
  std::string lang;
  std::string voice;
  std::string text;
};

std::optional<Request> parseRequest(const std::string& line)
{
  std::istringstream fields(line);
  Request request;
  if (!std::getline(fields, request.id, '\t') || !std::getline(fields, request.lang, '\t') ||
      !std::getline(fields, request.voice, '\t') || !std::getline(fields, request.text))
    return std::nullopt;
  if ((request.lang != "es" && request.lang != "en") || request.voice.empty() || request.text.empty())
    return std::nullopt;
  return request;
}

}

int main(int argc, char** argv)
{
  const auto options = parseOptions(std::span(argv, static_cast<std::size_t>(argc)));
  if (!options) {
    std::cerr << "usage: argus-tts-batch --config <config.toml> --models <models/tts> --out <dir> [--threads <n>]"
                 " < requests (lines: id<TAB>lang<TAB>voice<TAB>text)\n";
    return 2;
  }
  ConfigService::load(options->config.string());
  ConfigService::setRuntimeString("tts.models_dir", options->models.string());
  ConfigService::setRuntimeString("tts.threads", options->threads);
  ConfigService::setRuntimeString("tts.normalize_text", "true");
  ConfigService::setRuntimeString("tts.engine_es", "supertonic");
  ConfigService::setRuntimeString("tts.engine_en", "supertonic");
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
    const auto request = parseRequest(line);
    if (!request) {
      std::cerr << "malformed request line\n";
      ++failures;
      continue;
    }
    const auto samples = service.synthesize({.text = request->text,
                                             .lang = request->lang == "es" ? TtsLang::ES : TtsLang::EN,
                                             .voiceId = request->voice,
                                             .quality = TtsQuality::Auto,
                                             .speed = 1.0F});
    if (samples.empty()) {
      std::cerr << request->id << ": no audio\n";
      ++failures;
      continue;
    }
    wave_writer::write(options->out / (request->id + ".wav"), samples, service.sampleRate());
    std::cout << request->id << '\n' << std::flush;
  }
  service.shutdown();
  return failures == 0 ? 0 : 1;
}
