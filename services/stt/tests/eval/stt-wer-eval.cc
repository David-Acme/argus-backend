#include <config/config-service.hxx>
#include <feature/stt/services/stt-service.hxx>

#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr int kSkipped = 77;
constexpr int kSampleRate = 16000;

struct Options
{
  std::string manifest;
  std::string gates;
  std::string models;
  std::string report;
  std::string dump;
};

struct Clip
{
  std::string set;
  std::string id;
  std::string pcm;
  std::string reference;
  std::string meta;
};

struct Edits
{
  size_t errors = 0;
  size_t reference = 0;
};

struct SetTotals
{
  Edits words;
  Edits wordsNoAccent;
  Edits characters;
  size_t clips = 0;
  double seconds = 0.0;
  double decodeSeconds = 0.0;
};

Options parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string arg = argv[i];
    if (arg == "--manifest")
      options.manifest = argv[i + 1];
    else if (arg == "--gates")
      options.gates = argv[i + 1];
    else if (arg == "--models")
      options.models = argv[i + 1];
    else if (arg == "--report")
      options.report = argv[i + 1];
    else if (arg == "--dump")
      options.dump = argv[i + 1];
  }
  return options;
}

std::vector<char32_t> codepoints(const std::string& text)
{
  std::vector<char32_t> out;
  for (size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    size_t length = 1;
    char32_t value = lead;
    if (lead >= 0xF0) {
      length = 4;
      value = lead & 0x07U;
    }
    else if (lead >= 0xE0) {
      length = 3;
      value = lead & 0x0FU;
    }
    else if (lead >= 0xC0) {
      length = 2;
      value = lead & 0x1FU;
    }
    for (size_t k = 1; k < length && i + k < text.size(); ++k)
      value = (value << 6U) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
    out.push_back(value);
    i += length;
  }
  return out;
}

char32_t lowered(char32_t c)
{
  if (c >= 'A' && c <= 'Z')
    return c + 0x20;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
    return c + 0x20;
  return c;
}

char32_t unaccented(char32_t c)
{
  switch (c) {
    case 0xE1: case 0xE0: case 0xE2: case 0xE4:
      return 'a';
    case 0xE9: case 0xE8: case 0xEA: case 0xEB:
      return 'e';
    case 0xED: case 0xEC: case 0xEE: case 0xEF:
      return 'i';
    case 0xF3: case 0xF2: case 0xF4: case 0xF6:
      return 'o';
    case 0xFA: case 0xF9: case 0xFB: case 0xFC:
      return 'u';
    case 0xF1:
      return 'n';
    default:
      return c;
  }
}

bool keepsCodepoint(char32_t c)
{
  if (c < 0x80)
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
  return (c >= 0xC0 && c <= 0x24F) && c != 0xD7 && c != 0xF7;
}

std::vector<char32_t> normalized(const std::string& text, bool stripAccents)
{
  std::vector<char32_t> out;
  bool pendingSpace = false;
  for (const char32_t original : codepoints(text)) {
    char32_t c = lowered(original);
    if (stripAccents)
      c = unaccented(c);
    if (c == ' ' || c == '\t' || c == '\n' || c == '-' || c == 0x2014 || c == 0x2013) {
      pendingSpace = !out.empty();
      continue;
    }
    if (!keepsCodepoint(c))
      continue;
    if (pendingSpace)
      out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
  }
  return out;
}

std::vector<std::u32string> words(const std::vector<char32_t>& chars)
{
  std::vector<std::u32string> out;
  std::u32string current;
  for (const char32_t c : chars) {
    if (c == ' ') {
      if (!current.empty())
        out.push_back(std::move(current));
      current.clear();
    }
    else
      current.push_back(c);
  }
  if (!current.empty())
    out.push_back(std::move(current));
  return out;
}

template <typename T>
size_t editDistance(const std::vector<T>& a, const std::vector<T>& b)
{
  std::vector<size_t> previous(b.size() + 1);
  std::vector<size_t> current(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    previous[j] = j;
  for (size_t i = 1; i <= a.size(); ++i) {
    current[0] = i;
    for (size_t j = 1; j <= b.size(); ++j) {
      const size_t substitution = previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[b.size()];
}

std::optional<std::vector<Clip>> readManifest(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  std::vector<Clip> clips;
  std::string line;
  std::getline(in, line);
  while (std::getline(in, line)) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t'))
      fields.push_back(field);
    if (fields.size() < 5)
      continue;
    clips.push_back({.set = fields[0], .id = fields[1], .pcm = fields[2], .reference = fields[3], .meta = fields[4]});
  }
  return clips;
}

std::vector<float> readPcm(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::vector<float> samples;
  int16_t value = 0;
  while (in.read(reinterpret_cast<char*>(&value), sizeof(value)))
    samples.push_back(static_cast<float>(value) / 32768.0F);
  return samples;
}

double ratio(size_t numerator, size_t denominator)
{
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

struct Verdict
{
  std::vector<std::string> failures;
};

Verdict check(const std::string& gatesPath, const std::map<std::string, double>& metrics)
{
  Verdict verdict;
  std::ifstream in(gatesPath);
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  if (!in || !Json::parseFromStream(builder, in, &root, &errors)) {
    verdict.failures.push_back("cannot read gates " + gatesPath);
    return verdict;
  }
  const Json::Value& gates = root["wer"]["metrics"];
  for (const auto& name : gates.getMemberNames()) {
    const auto found = metrics.find(name);
    if (found == metrics.end()) {
      verdict.failures.push_back(name + ": gated but not measured");
      continue;
    }
    if (gates[name].isMember("max") && found->second > gates[name]["max"].asDouble() + 1e-9)
      verdict.failures.push_back(name + ": " + std::to_string(found->second) + " above " +
                                 std::to_string(gates[name]["max"].asDouble()));
    if (gates[name].isMember("min") && found->second < gates[name]["min"].asDouble() - 1e-9)
      verdict.failures.push_back(name + ": " + std::to_string(found->second) + " below " +
                                 std::to_string(gates[name]["min"].asDouble()));
  }
  return verdict;
}

}

int main(int argc, char** argv)
{
  const Options options = parseOptions(argc, argv);
  const auto clips = readManifest(options.manifest);
  if (!clips || clips->empty()) {
    std::printf("[SKIPPED] no STT evaluation clips at %s; run scripts/stt-eval-data.py\n", options.manifest.c_str());
    return kSkipped;
  }
  if (!std::filesystem::exists(options.models + "/nemo-transducer-encoder.int8.onnx")) {
    std::printf("[SKIPPED] no nemo transducer model under %s\n", options.models.c_str());
    return kSkipped;
  }

  ConfigService::setRuntimeString("stt.engine", "nemo_transducer");
  ConfigService::setRuntimeString("stt.language", "es");
  ConfigService::setRuntimeString("stt.models_dir", options.models);
  SttService::instance().init();
  if (!SttService::instance().isLoaded()) {
    std::printf("[ERROR] the STT engine did not load from %s\n", options.models.c_str());
    return 1;
  }

  const std::filesystem::path root = std::filesystem::path(options.manifest).parent_path();
  std::map<std::string, SetTotals> totals;
  SetTotals overall;
  std::ofstream dump;
  if (!options.dump.empty())
    dump.open(options.dump);
  for (const auto& clip : *clips) {
    const std::vector<float> samples = readPcm(root / clip.pcm);
    if (samples.empty())
      continue;
    const auto started = std::chrono::steady_clock::now();
    const std::string hypothesis = SttService::instance().transcribe({.samples = samples, .sampleRate = kSampleRate, .lang = "es"});
    const double decode = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const std::vector<char32_t> refStrict = normalized(clip.reference, false);
    const std::vector<char32_t> hypStrict = normalized(hypothesis, false);
    const auto refWords = words(refStrict);
    const auto hypWords = words(hypStrict);
    const auto refPlain = words(normalized(clip.reference, true));
    const auto hypPlain = words(normalized(hypothesis, true));
    const size_t wordErrors = editDistance(refWords, hypWords);
    const size_t plainErrors = editDistance(refPlain, hypPlain);
    const size_t charErrors = editDistance(refStrict, hypStrict);
    const double seconds = static_cast<double>(samples.size()) / kSampleRate;
    for (SetTotals* bucket : {&totals[clip.set], &overall}) {
      bucket->words.errors += wordErrors;
      bucket->words.reference += refWords.size();
      bucket->wordsNoAccent.errors += plainErrors;
      bucket->wordsNoAccent.reference += refPlain.size();
      bucket->characters.errors += charErrors;
      bucket->characters.reference += refStrict.size();
      bucket->clips += 1;
      bucket->seconds += seconds;
      bucket->decodeSeconds += decode;
    }
    if (dump)
      dump << clip.set << '\t' << clip.id << '\t' << wordErrors << '/' << refWords.size() << '\t' << clip.meta
           << '\t' << clip.reference << '\t' << hypothesis << '\n';
  }

  std::map<std::string, double> metrics;
  const auto record = [&metrics](const std::string& name, const SetTotals& total) {
    metrics[name + ".clips"] = static_cast<double>(total.clips);
    metrics[name + ".wer"] = ratio(total.words.errors, total.words.reference);
    metrics[name + ".werNoAccent"] = ratio(total.wordsNoAccent.errors, total.wordsNoAccent.reference);
    metrics[name + ".cer"] = ratio(total.characters.errors, total.characters.reference);
    metrics[name + ".realTimeFactor"] = total.seconds > 0.0 ? total.decodeSeconds / total.seconds : 0.0;
  };
  for (const auto& [set, total] : totals)
    record(set, total);
  record("overall", overall);
  std::printf("stt wer: %zu clips, %.1f s of audio\n", overall.clips, overall.seconds);
  for (const auto& [name, value] : metrics)
    std::printf("  %-44s %.4f\n", name.c_str(), value);

  const Verdict verdict = check(options.gates, metrics);
  if (!options.report.empty()) {
    Json::Value out(Json::objectValue);
    for (const auto& [name, value] : metrics)
      out["metrics"][name] = value;
    out["passed"] = verdict.failures.empty();
    std::ofstream file(options.report);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    file << Json::writeString(builder, out) << "\n";
  }
  SttService::instance().shutdown();
  if (!verdict.failures.empty()) {
    std::printf("GATE FAILED\n");
    for (const auto& failure : verdict.failures)
      std::printf("  %s\n", failure.c_str());
    std::fflush(stdout);
    return 1;
  }
  std::printf("GATE PASSED\n");
  std::fflush(stdout);
  return 0;
}
