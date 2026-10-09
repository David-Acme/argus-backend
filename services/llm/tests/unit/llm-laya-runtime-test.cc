#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>
#include <feature/llm/services/turn/laya-sequence.hxx>
#include <feature/llm/services/turn/sp-tokenizer.hxx>
#include <feature/llm/services/turn/turn-flow.hxx>
#include "tool-stubs.hxx"

#include <json/reader.h>
#include <json/value.h>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

struct Pilot
{
  fs::path dir;
  std::string pin;
};

std::optional<Pilot> pilotBundle()
{
  const char* dir = std::getenv("ARGUS_LAYA_BUNDLE");
  const char* pin = std::getenv("ARGUS_LAYA_PIN");
  if (dir == nullptr || pin == nullptr || !fs::exists(fs::path(dir) / "model.onnx"))
    return std::nullopt;
  return Pilot{.dir = fs::path(dir), .pin = pin};
}

Json::Value readJson(const fs::path& path)
{
  std::ifstream in(path);
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  Json::parseFromStream(builder, in, &root, &errors);
  return root;
}

fs::path onnxReferencePath()
{
  fs::path path(ARGUS_TEST_LAYA_PILOT);
  path.replace_filename("parity-onnx.json");
  return path;
}

turn::BundleDecode decodeOf(const Json::Value& parameters)
{
  turn::BundleDecode decode;
  if (parameters["headMaxLen"].isInt() && parameters["headMaxLen"].asInt() > 0)
    decode.headMaxLen = parameters["headMaxLen"].asInt();
  const Json::Value& temperature = parameters["temperature"];
  for (Json::ArrayIndex index = 0; index < temperature.size() && index < decode.temperature.size(); ++index)
    decode.temperature[index] = temperature[index].asDouble();
  return decode;
}

bool knob(const char* name)
{
  const char* value = std::getenv(name);
  return value != nullptr && std::string(value) != "0";
}

turn::OnnxOptions optionsOf()
{
  turn::OnnxOptions options;
  options.cpuArena = knob("ARGUS_LAYA_RSS_CPU_ARENA");
  options.memPattern = knob("ARGUS_LAYA_RSS_MEM_PATTERN");
  options.prepacking = knob("ARGUS_LAYA_RSS_PREPACKING");
  const char* mmap = std::getenv("ARGUS_LAYA_RSS_MMAP");
  options.mmap = mmap == nullptr || std::string(mmap) != "0";
  return options;
}

struct World
{
  ToolRegistry registry;
  ToolExecutor executor{registry};
  slots::RuleText text;
  std::vector<tools::ToolCall> ran;
  std::vector<tools::ToolHandle> offered;
  ToolAudience audience{.role = UserRole::Owner, .modules = {}};
  tools::ToolContext context{.userId = 24,
                             .role = UserRole::Owner,
                             .lang = "es",
                             .sessionId = "laya-runtime",
                             .channel = "tool_result",
                             .utterance = {},
                             .decided = false,
                             .turn = 1,
                             .emitAction = {}};

  World()
  {
    for (const turn::LayaLabel& label : turn::layaLabels())
      registry.registerTool(tool_stubs::stub({.name = std::string(label.tool),
                                              .capability = "notifications.read",
                                              .handler = [this](const tools::ToolCall& call) {
                                                ran.push_back(call);
                                                return tool_stubs::okResult("hecho");
                                              }}));
    offered = registry.all();
  }
};

std::optional<std::string> toolOfLabel(std::string_view label)
{
  const std::string_view tool = turn::layaToolFor(label);
  return tool.empty() ? std::nullopt : std::optional<std::string>(tool);
}

std::string topLabel(const Json::Value& probabilities)
{
  std::string best;
  double score = -1.0;
  for (const std::string& label : probabilities.getMemberNames()) {
    if (label == "none" || label == "ask")
      continue;
    if (probabilities[label].asDouble() > score) {
      score = probabilities[label].asDouble();
      best = label;
    }
  }
  return best;
}
}

TEST_CASE("the real Laya bundle decides, and the turn runs the tool it chose")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no Laya pilot bundle at ARGUS_LAYA_BUNDLE and ARGUS_LAYA_PIN; the Laya runtime did not run");
    return;
  }
  const Json::Value fixture = readJson(ARGUS_TEST_LAYA_PILOT);
  const Json::Value reference = readJson(onnxReferencePath());
  REQUIRE(reference["cases"].size() == fixture["cases"].size());
  CHECK(reference["bundlePin"].asString() == pilot->pin);
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin});
  REQUIRE(bundle.valid());
  World world;
  turn::LayaDecider laya({.model =
                              turn::openLayaModel({.bundle = bundle, .options = optionsOf(), .decode = decodeOf(fixture["parameters"])}),
                          .fallback = nullptr,
                          .policy = bundle.policy(),
                          .confidence = bundle.confidenceCalibration(),
                          .now = bundle.nowCalibration()});
  CHECK(laya.model_ready());
  std::size_t decided = 0;
  std::size_t engaged = 0;
  std::size_t acted = 0;
  for (Json::ArrayIndex index = 0; index < fixture["cases"].size(); ++index) {
    const Json::Value& item = fixture["cases"][index];
    const Json::Value& held = reference["cases"][index];
    REQUIRE(held["text"].asString() == item["text"].asString());
    const std::string text = item["text"].asString();
    const std::optional<std::string> expected = toolOfLabel(topLabel(held["tool"]));
    REQUIRE(expected.has_value());
    const turn::DecideInput input{.utterance = text,
                                  .lang = "es",
                                  .offered = world.offered,
                                  .modules = world.audience.modules,
                                  .previousAssistant = {}};
    const std::optional<turn::Candidate> candidate = laya.decide(input);
    REQUIRE(candidate.has_value());
    CHECK(candidate->tool == *expected);
    CHECK(candidate->decider == "laya");
    ++decided;
    turn::TurnFlow flow({.executor = world.executor,
                         .decider = &laya,
                         .text = &world.text,
                         .policies = turn::PolicySet(turn::DecisionPolicy{.act = bundle.policy().act,
                                                                          .ask = bundle.policy().ask,
                                                                          .margin = bundle.policy().margin,
                                                                          .nowMin = bundle.policy().now})});
    const turn::Outcome outcome = flow.run({.utterance = text,
                                            .offered = world.offered,
                                            .audience = world.audience,
                                            .context = world.context,
                                            .now = 0,
                                            .previousAssistant = {}});
    if (!outcome.decidedTool.empty())
      CHECK(outcome.decidedTool == *expected);
    if (!outcome.decidedTool.empty() || outcome.question.has_value() || !outcome.steps.empty())
      ++engaged;
    for (const turn::Step& step : outcome.steps) {
      CHECK(step.call.name == *expected);
      ++acted;
    }
  }
  MESSAGE("laya turn: " << decided << " decisions, " << engaged << " engaged, " << acted << " tools run, " << world.ran.size()
                        << " calls recorded");
  CHECK(decided == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(engaged >= 1);
}

TEST_CASE("the Laya scores the bundle produces match the Python reference")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no Laya pilot bundle at ARGUS_LAYA_BUNDLE and ARGUS_LAYA_PIN; the Laya parity did not run");
    return;
  }
  const Json::Value fixture = readJson(ARGUS_TEST_LAYA_PILOT);
  const Json::Value reference = readJson(onnxReferencePath());
  REQUIRE(reference["cases"].size() == fixture["cases"].size());
  CHECK(reference["bundlePin"].asString() == pilot->pin);
  CHECK(reference["modelSha256"].isString());
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin});
  REQUIRE(bundle.valid());
  turn::LayaDecider laya({.model = turn::openLayaModel({.bundle = bundle, .options = {}, .decode = decodeOf(fixture["parameters"])}),
                          .fallback = nullptr,
                          .policy = bundle.policy(),
                          .confidence = {},
                          .now = {}});
  REQUIRE(laya.model_ready());
  double worstProbability = 0.0;
  double worstNow = 0.0;
  std::size_t rows = 0;
  std::size_t decisionMismatches = 0;
  for (Json::ArrayIndex index = 0; index < fixture["cases"].size(); ++index) {
    const Json::Value& item = fixture["cases"][index];
    const Json::Value& held = reference["cases"][index];
    REQUIRE(held["text"].asString() == item["text"].asString());
    const std::optional<turn::LayaReading> reading = laya.read(item["text"].asString(), "es");
    REQUIRE(reading.has_value());
    const Json::Value& expected = held["tool"];
    double total = 0.0;
    for (const std::string& label : expected.getMemberNames()) {
      const auto found =
          std::ranges::find(reading->probabilities, label, &std::pair<std::string, double>::first);
      REQUIRE(found != reading->probabilities.end());
      const double gap = std::abs(found->second - expected[label].asDouble());
      worstProbability = std::max(worstProbability, gap);
      total += found->second;
    }
    CHECK(total == doctest::Approx(1.0).epsilon(0.001));
    const auto best = std::ranges::max_element(reading->probabilities, {}, &std::pair<std::string, double>::second);
    REQUIRE(best != reading->probabilities.end());
    if (best->first != held["choice"].asString())
      ++decisionMismatches;
    worstNow = std::max(worstNow, std::abs(reading->now - held["now"].asDouble()));
    ++rows;
  }
  MESSAGE("laya parity vs " << reference["backend"].asString() << " on " << reference["modelSha256"].asString().substr(0, 12)
                            << ": " << rows << " rows, worst probability gap " << worstProbability << ", worst now gap "
                            << worstNow << ", " << decisionMismatches << " decisions differing");
  CHECK(rows == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(worstProbability <= 2e-3);
  CHECK(worstNow <= 2e-3);
  CHECK(decisionMismatches == 0);
}

TEST_CASE("the Laya decode reproduces the reference token ids and sequences")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no Laya pilot bundle at ARGUS_LAYA_BUNDLE and ARGUS_LAYA_PIN; the Laya sequence parity did not run");
    return;
  }
  const Json::Value fixture = readJson(ARGUS_TEST_LAYA_PILOT);
  turn::SpTokenizer tokenizer;
  REQUIRE(tokenizer.load(pilot->dir / "tokenizer" / "tokenizer.json"));
  const Json::Value questions = readJson(pilot->dir / "questions.json");
  const int headMaxLen = fixture["parameters"]["headMaxLen"].asInt();
  std::size_t rows = 0;
  std::size_t mismatches = 0;
  for (const Json::Value& item : fixture["cases"]) {
    std::vector<std::int32_t> expectedTokens;
    for (const Json::Value& token : item["tokens"])
      expectedTokens.push_back(token.asInt());
    if (tokenizer.encode(item["text"].asString(), false) != expectedTokens)
      ++mismatches;
    ++rows;
  }
  CHECK(rows == static_cast<std::size_t>(fixture["cases"].size()));
  CHECK(mismatches == 0);
  const std::optional<turn::LayaQuestion> question = turn::layaQuestionFromJson(questions["tool"]);
  REQUIRE(question.has_value());
  const turn::LayaSequence sequence =
      turn::layaBuildSequence({.tokenizer = tokenizer, .state = "recuérdame comprar pan mañana a las ocho", .maxLen = 1024, .headMaxLen = headMaxLen},
                              *question);
  CHECK(sequence.markers.size() == 22);
  CHECK(sequence.ids.size() <= 1024);
}

TEST_CASE("the warm resident set the Laya model adds is measured in service")
{
  const std::optional<Pilot> pilot = pilotBundle();
  if (!pilot) {
    MESSAGE("no Laya pilot bundle at ARGUS_LAYA_BUNDLE and ARGUS_LAYA_PIN; the warm resident reading did not run");
    return;
  }
  const turn::BundleLoader bundle({.dir = pilot->dir, .pin = pilot->pin});
  REQUIRE(bundle.valid());
  const turn::OnnxOptions options = optionsOf();
  const std::int64_t before = turn::residentBytes();
  turn::LayaDecider laya({.model = turn::openLayaModel({.bundle = bundle, .options = options, .decode = bundle.decode()}),
                          .fallback = nullptr,
                          .policy = bundle.policy(),
                          .confidence = {},
                          .now = {}});
  REQUIRE(laya.model_ready());
  const std::int64_t delta = laya.warmRssBytes();
  const double mb = static_cast<double>(delta) / (1024.0 * 1024.0);
  const std::optional<turn::LayaReading> reading = laya.read("recuérdame comprar pan mañana a las ocho", "es");
  REQUIRE(reading.has_value());
  const double warmMb = static_cast<double>(turn::residentBytes() - before) / (1024.0 * 1024.0);
  MESSAGE("laya warm rss: cpu_arena=" << options.cpuArena << " mem_pattern=" << options.memPattern << " prepacking=" << options.prepacking
                                      << " mmap=" << options.mmap << " session load delta " << mb << " MB, process warm delta " << warmMb
                                      << " MB");
  CHECK(delta > 0);
}
