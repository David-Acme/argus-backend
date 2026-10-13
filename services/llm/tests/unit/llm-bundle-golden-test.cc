#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/bundle-golden.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/laya-sequence.hxx>
#include <feature/llm/services/turn/sp-tokenizer.hxx>

#include <text/sha256.hxx>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

const char* kBpe = R"({
 "added_tokens": [
  {"id": 0, "content": "<pad>", "special": true},
  {"id": 1, "content": "<eos>", "special": true},
  {"id": 2, "content": "<bos>", "special": true},
  {"id": 3, "content": "<unk>", "special": true},
  {"id": 4, "content": "<mask>", "special": true}
 ],
 "normalizer": {"type": "Replace", "pattern": {"String": " "}, "content": "▁"},
 "pre_tokenizer": {"type": "Metaspace", "replacement": "▁", "prepend_scheme": "always", "split": true},
 "model": {
  "type": "BPE", "byte_fallback": true, "ignore_merges": false, "fuse_unk": true, "unk_token": "<unk>",
  "vocab": {"<pad>": 0, "<eos>": 1, "<bos>": 2, "<unk>": 3, "<mask>": 4, "▁": 5, "h": 6, "o": 7,
            "l": 8, "a": 9, "▁h": 10, "▁ho": 11, "▁hol": 12, "▁hola": 13},
  "merges": [["▁", "h"], ["▁h", "o"], ["▁ho", "l"], ["▁hol", "a"]]
 }
})";

const char* kQuestions = R"({"tool": {"type": "choice", "instructions": "hola", "criteria": ["memory_save", "camera"]},
 "now": {"type": "noul", "instructions": "hola"}})";

class FixedLaya final : public turn::LayaModel
{
public:
  explicit FixedLaya(turn::LayaReading reading) : reading_(std::move(reading)) {}

  [[nodiscard]] turn::EngineStatus status() override { return turn::EngineStatus::Ready; }

  [[nodiscard]] std::optional<turn::LayaReading> read(std::string_view, std::string_view) override { return reading_; }

private:
  turn::LayaReading reading_;
};

void write(const fs::path& path, const std::string& bytes)
{
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << bytes;
}

std::string readText(const fs::path& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void seal(const fs::path& dir)
{
  std::vector<std::string> names;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir))
    if (entry.is_regular_file() && entry.path().filename() != "sha256")
      names.push_back(entry.path().lexically_relative(dir).generic_string());
  std::ranges::sort(names);
  std::string listing;
  for (const std::string& name : names)
    listing += argus::hash::sha256Hex(readText(dir / name)) + "  " + name + "\n";
  write(dir / "sha256", listing);
}

nlohmann::ordered_json sequenceOf(const fs::path& dir, std::string_view qid)
{
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(dir / "tokenizer" / "tokenizer.json"));
  const std::optional<turn::LayaQuestion> question = turn::layaQuestionFromJson(nlohmann::ordered_json::parse(kQuestions).at(qid));
  REQUIRE(question.has_value());
  const turn::LayaSequence built = turn::layaBuildSequence({.tokenizer = tokenizer, .state = "hola", .maxLen = 64, .headMaxLen = 32},
                                                           question.value_or(turn::LayaQuestion{}));
  return {{"ids", built.ids}, {"markers", built.markers}};
}

struct Layout
{
  std::string name;
  std::string precision;
  double probabilityTolerance{0.002};
};

fs::path bundleWith(const Layout& layout, const nlohmann::ordered_json& probe)
{
  const fs::path dir = fs::temp_directory_path() / ("argus-golden-" + layout.name);
  fs::remove_all(dir);
  write(dir / "model.onnx", "weights");
  write(dir / "tokenizer" / "tokenizer.json", kBpe);
  write(dir / "labels.json", R"({"labels": ["memory_save", "camera"]})");
  write(dir / "decision.json", R"({"act": 0.8, "ask": 0.8, "head_max_len": 32, "temperature": [1.0, 1.0, 1.0]})");
  write(dir / "max_len", "64\n");
  write(dir / "model-card.md", "# card\n");
  write(dir / "questions.json", kQuestions);
  nlohmann::ordered_json withSequences = probe;
  if (!withSequences.contains("sequences"))
    withSequences["sequences"] = {{"tool", sequenceOf(dir, "tool")}, {"now", sequenceOf(dir, "now")}};
  write(dir / "golden.json", nlohmann::ordered_json{{"kind", "laya"}, {"probes", {withSequences}}}.dump());
  nlohmann::ordered_json manifest = {{"model", "laya"},
                                     {"calibration", {{"fitOn", "calibration"}}},
                                     {"modelOnnx", {{"sha256", argus::hash::sha256Hex("weights")}}},
                                     {"questions", {{"file", "questions.json"}, {"sha256", argus::hash::sha256Hex(kQuestions)}}},
                                     {"golden",
                                      {{"file", "golden.json"},
                                       {"sha256", argus::hash::sha256Hex(readText(dir / "golden.json"))},
                                       {"tolerance", {{"probability", layout.probabilityTolerance}, {"now", 0.002}}}}}};
  if (!layout.precision.empty())
    manifest["precision"] = layout.precision;
  write(dir / "manifest.json", manifest.dump());
  seal(dir);
  return dir;
}

turn::LayaReading readingOf(double save, double now)
{
  return {.probabilities = {{"memory_save", save}, {"camera", 1.0 - save}}, .now = now};
}

nlohmann::ordered_json probeOf(double save, double now)
{
  return {{"text", "hola"}, {"lang", "es"}, {"tool", {{"memory_save", save}, {"camera", 1.0 - save}}}, {"choice", save >= 0.5 ? "memory_save" : "camera"}, {"now", now}};
}
}

TEST_CASE("an int8 bundle whose model reproduces its own golden vectors within the tolerance passes")
{
  const turn::BundleLoader bundle({.dir = bundleWith({.name = "pass", .precision = "int8"}, probeOf(0.9, 0.7))});
  REQUIRE(bundle.valid());
  CHECK(bundle.precision() == turn::BundlePrecision::Int8);
  REQUIRE(bundle.golden().present);
  FixedLaya model(readingOf(0.901, 0.7005));
  const turn::GoldenReport report = turn::checkLayaGolden({.bundle = bundle, .model = model});
  CHECK(report.passed);
  CHECK(report.probes == 1);
  CHECK(report.sequenceMismatches == 0);
}

TEST_CASE("a score outside the bundle's own tolerance fails, and so does a changed decision")
{
  const turn::BundleLoader bundle({.dir = bundleWith({.name = "drift", .precision = "fp32"}, probeOf(0.9, 0.7))});
  REQUIRE(bundle.valid());
  FixedLaya drifted(readingOf(0.89, 0.7));
  CHECK_FALSE(turn::checkLayaGolden({.bundle = bundle, .model = drifted}).passed);
  FixedLaya flipped(readingOf(0.4, 0.7));
  const turn::GoldenReport report = turn::checkLayaGolden({.bundle = bundle, .model = flipped});
  CHECK_FALSE(report.passed);
  CHECK(report.decisionMismatches == 1);
}

TEST_CASE("a golden sequence the renderer does not reproduce fails, whatever the scores")
{
  nlohmann::ordered_json probe = probeOf(0.9, 0.7);
  probe["sequences"] = {{"tool", {{"ids", {2, 13, 1}}, {"markers", {1}}}}};
  const turn::BundleLoader bundle({.dir = bundleWith({.name = "sequence", .precision = "fp32"}, probe)});
  REQUIRE(bundle.valid());
  FixedLaya model(readingOf(0.9, 0.7));
  const turn::GoldenReport report = turn::checkLayaGolden({.bundle = bundle, .model = model});
  CHECK_FALSE(report.passed);
  CHECK(report.sequenceMismatches == 1);
}

TEST_CASE("the precision is declared, read from the legacy artefact kind, or refused")
{
  CHECK(turn::BundleLoader({.dir = bundleWith({.name = "fp32", .precision = "fp32"}, probeOf(0.9, 0.7))}).precision() ==
        turn::BundlePrecision::Fp32);
  const turn::BundleLoader missing({.dir = bundleWith({.name = "unknown", .precision = ""}, probeOf(0.9, 0.7))});
  CHECK_FALSE(missing.valid());
  CHECK(missing.error().find("declares no precision") != std::string::npos);
  const turn::BundleLoader other({.dir = bundleWith({.name = "bf16", .precision = "bf16"}, probeOf(0.9, 0.7))});
  CHECK_FALSE(other.valid());
  const fs::path legacy = bundleWith({.name = "legacy", .precision = ""}, probeOf(0.9, 0.7));
  nlohmann::ordered_json manifest = nlohmann::ordered_json::parse(readText(legacy / "manifest.json"));
  manifest["artefactKind"] = "int8-embedding";
  write(legacy / "manifest.json", manifest.dump());
  seal(legacy);
  const turn::BundleLoader kind({.dir = legacy});
  REQUIRE(kind.valid());
  CHECK(kind.precision() == turn::BundlePrecision::Int8);
}

TEST_CASE("golden vectors the manifest records under another hash are refused at load")
{
  const fs::path dir = bundleWith({.name = "tampered", .precision = "int8"}, probeOf(0.9, 0.7));
  write(dir / "golden.json", nlohmann::ordered_json{{"kind", "laya"}, {"probes", {probeOf(0.2, 0.7)}}}.dump());
  seal(dir);
  const turn::BundleLoader bundle({.dir = dir});
  CHECK_FALSE(bundle.valid());
  CHECK(bundle.error().find("golden vectors") != std::string::npos);
}

TEST_CASE("a bundle without golden vectors loads, and its model is not checked")
{
  const fs::path dir = bundleWith({.name = "bare", .precision = "fp32"}, probeOf(0.9, 0.7));
  nlohmann::ordered_json manifest = nlohmann::ordered_json::parse(readText(dir / "manifest.json"));
  manifest.erase("golden");
  write(dir / "manifest.json", manifest.dump());
  fs::remove(dir / "golden.json");
  seal(dir);
  const turn::BundleLoader bundle({.dir = dir});
  REQUIRE(bundle.valid());
  CHECK_FALSE(bundle.golden().present);
  FixedLaya model(readingOf(0.9, 0.7));
  const turn::GoldenReport report = turn::checkLayaGolden({.bundle = bundle, .model = model});
  CHECK_FALSE(report.passed);
  CHECK(report.error == "the bundle carries no golden vectors");
}
