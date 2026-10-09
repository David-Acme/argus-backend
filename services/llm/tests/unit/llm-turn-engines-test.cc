#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>
#include <feature/llm/services/turn/turn-flow.hxx>
#include <mcp/schema.hxx>
#include "tool-stubs.hxx"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

fs::path pilotBundle(std::string_view kind)
{
  return fs::path(ARGUS_TEST_BUNDLES_DIR) / std::string(kind) / "pilot";
}

class StubLayaModel final : public turn::LayaModel
{
public:
  StubLayaModel(turn::EngineStatus status, std::vector<std::pair<std::string, double>> probabilities, double now)
      : status_(status), probabilities_(std::move(probabilities)), now_(now)
  {
  }

  [[nodiscard]] turn::EngineStatus status() override { return status_; }

  [[nodiscard]] std::optional<turn::LayaReading> read(std::string_view, std::string_view) override
  {
    ++reads_;
    if (status_ != turn::EngineStatus::Ready)
      return std::nullopt;
    return turn::LayaReading{.probabilities = probabilities_, .now = now_};
  }

  mutable int reads_{0};

private:
  turn::EngineStatus status_;
  std::vector<std::pair<std::string, double>> probabilities_;
  double now_{0.0};
};

class StubGlinerModel final : public turn::GlinerModel
{
public:
  StubGlinerModel(turn::EngineStatus status, std::vector<turn::GlinerSpan> spans) : status_(status), spans_(std::move(spans)) {}

  [[nodiscard]] turn::EngineStatus status() override { return status_; }

  [[nodiscard]] std::vector<turn::GlinerSpan> spans(const turn::GlinerRequest&) override
  {
    return status_ == turn::EngineStatus::Ready ? spans_ : std::vector<turn::GlinerSpan>{};
  }

private:
  turn::EngineStatus status_;
  std::vector<turn::GlinerSpan> spans_;
};

class CountingDecider final : public turn::Decider
{
public:
  CountingDecider(std::string name, std::optional<turn::Candidate> candidate)
      : name_(std::move(name)), candidate_(std::move(candidate))
  {
  }

  [[nodiscard]] std::string_view id() const override { return name_; }

  [[nodiscard]] std::optional<turn::Candidate> decide(const turn::DecideInput&) const override
  {
    ++asked;
    return candidate_;
  }

  mutable int asked{0};

private:
  std::string name_;
  std::optional<turn::Candidate> candidate_;
};

turn::Candidate candidate(std::string tool, std::string decider, double confidence)
{
  return {.tool = std::move(tool),
          .arguments = Json::Value(Json::objectValue),
          .fill = {},
          .confidence = confidence,
          .source = decider,
          .decider = std::move(decider),
          .exact = false,
          .confident = true,
          .runnerUp = std::nullopt};
}

struct World
{
  ToolRegistry registry;
  ToolExecutor executor{registry};
  slots::RuleText text;
  std::vector<tools::ToolCall> ran;
  std::vector<tools::ToolHandle> offered;
  ToolAudience audience{.role = UserRole::Owner, .modules = {}};
  tools::ToolContext context{.userId = 7,
                             .role = UserRole::Owner,
                             .lang = "es",
                             .sessionId = "u24-1",
                             .channel = "tool_result",
                             .utterance = {},
                             .decided = false,
                             .turn = 1,
                             .emitAction = {}};
  int64_t now{0};

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

turn::DecideInput inputFor(World& world, std::string_view utterance)
{
  return {.utterance = utterance,
          .lang = "es",
          .offered = world.offered,
          .modules = world.audience.modules,
          .previousAssistant = {}};
}
}

TEST_CASE("the pilot decide bundle loads and its labels all serve a tool")
{
  const turn::BundleLoader bundle({.dir = pilotBundle("decide"), .pin = std::string(R"(c43268a720b6a25f2c9c402cfc1b2a631846d861aab80cd3d621841b6b855217)")});
  REQUIRE(bundle.valid());
  CHECK(bundle.labels().size() == 22);
  CHECK(bundle.fitSplit() == "calibration");
  World world;
  CHECK_FALSE(turn::unknownLayaLabel(bundle.labels(), world.registry).has_value());
}

TEST_CASE("the label set the server serves is the pinned tsv, and the pilot bundle is inside it")
{
  std::ifstream in(ARGUS_TEST_LAYA_LABELS);
  REQUIRE(in.is_open());
  std::vector<std::pair<std::string, std::string>> rows;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    rows.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  const std::span<const turn::LayaLabel> labels = turn::layaLabels();
  REQUIRE(rows.size() == labels.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    CHECK(rows[index].first == labels[index].label);
    CHECK(rows[index].second == labels[index].tool);
  }
  std::set<std::string> server;
  for (const auto& row : rows)
    server.insert(row.first);
  server.insert("none");
  const turn::BundleLoader bundle({.dir = pilotBundle("decide"), .pin = std::string(R"(c43268a720b6a25f2c9c402cfc1b2a631846d861aab80cd3d621841b6b855217)")});
  REQUIRE(bundle.valid());
  for (const std::string& label : bundle.labels())
    CHECK(server.contains(label));
}

TEST_CASE("a label no tool serves is refused rather than routed")
{
  World world;
  const std::vector<std::string> labels{"memory_save", "holiday_planning"};
  const std::optional<std::string> stray = turn::unknownLayaLabel(labels, world.registry);
  REQUIRE(stray.has_value());
  CHECK(*stray == "holiday_planning");
}

TEST_CASE("with engine laya the judge arbitrates rules and laya, and the router never proposes")
{
  World world;
  CountingDecider router("router", std::nullopt);
  auto model = std::make_unique<StubLayaModel>(turn::EngineStatus::Ready,
                                               std::vector<std::pair<std::string, double>>{{"memory_save", 0.95}, {"none", 0.03}},
                                               0.9);
  StubLayaModel* probe = model.get();
  turn::LayaDecider laya({.model = std::move(model),
                          .fallback = &router,
                          .policy = {},
                          .confidence = {},
                          .now = {}});
  const turn::DecideInput input = inputFor(world, "recuerda que el codigo es AB12CD");
  const std::optional<turn::Candidate> candidate = laya.decide(input);
  REQUIRE(candidate.has_value());
  CHECK(candidate->tool == "memory.remember");
  CHECK(candidate->decider == "laya");
  CHECK(candidate->source == "laya, memory_save");
  CHECK(probe->reads_ == 1);
  CHECK(router.asked == 0);
}

TEST_CASE("with engine router the router proposes and laya is never consulted")
{
  World world;
  const turn::Candidate fromRouter = candidate("memory.remember", "router", 0.95);
  CountingDecider router("router", fromRouter);
  auto model = std::make_unique<StubLayaModel>(turn::EngineStatus::Ready, std::vector<std::pair<std::string, double>>{}, 0.0);
  StubLayaModel* probe = model.get();
  turn::LayaDecider laya({.model = std::move(model), .fallback = &router, .policy = {}, .confidence = {}, .now = {}});
  const std::optional<turn::Candidate> candidate = router.decide(inputFor(world, "recuerda que el codigo es AB12CD"));
  REQUIRE(candidate.has_value());
  CHECK(candidate->decider == "router");
  CHECK(router.asked == 1);
  CHECK(probe->reads_ == 0);
  CHECK(laya.decide(inputFor(world, "otra")).value_or(fromRouter).decider != "laya");
}

TEST_CASE("a laya model that cannot load forwards the turn to the router")
{
  World world;
  CountingDecider router("router", candidate("memory.remember", "router", 0.95));
  turn::LayaDecider laya({.model = std::make_unique<StubLayaModel>(turn::EngineStatus::Unavailable, std::vector<std::pair<std::string, double>>{}, 0.0),
                          .fallback = &router,
                          .policy = {},
                          .confidence = {},
                          .now = {}});
  const std::optional<turn::Candidate> candidate = laya.decide(inputFor(world, "recuerda que el codigo es AB12CD"));
  REQUIRE(candidate.has_value());
  CHECK(candidate->decider == "router");
  CHECK(router.asked == 1);
}

TEST_CASE("the gliner extractor answers a span, and falls back to nuextract when it cannot load")
{
  World world;
  const slots::TextRequest request{.tool = "calendar.create_event", .field = "title", .utterance = "agenda una cita con el dentista", .lang = "es"};
  turn::GlinerExtractor gliner({.model = std::make_unique<StubGlinerModel>(
                                    turn::EngineStatus::Ready,
                                    std::vector<turn::GlinerSpan>{{.field = "title", .text = "cita con el dentista", .score = 0.9, .begin = 0, .end = 10}}),
                                .fallback = nullptr,
                                .thresholds = {.threshold = 0.5, .maxSpanWidth = 12, .present = true}});
  CHECK(gliner.extract(request) == std::optional<std::string>("cita con el dentista"));
  turn::GlinerExtractor off({.model = std::make_unique<StubGlinerModel>(turn::EngineStatus::Unavailable, std::vector<turn::GlinerSpan>{}),
                             .fallback = nullptr,
                             .thresholds = {}});
  CHECK_FALSE(off.extract(request).has_value());
}

TEST_CASE("the whole new pipeline runs on the pilot bundles: laya decides, gliner fills, the tool runs")
{
  World world;
  world.now = 0;
  const turn::BundleLoader bundle({.dir = pilotBundle("decide"), .pin = std::string(R"(c43268a720b6a25f2c9c402cfc1b2a631846d861aab80cd3d621841b6b855217)")});
  REQUIRE(bundle.valid());
  turn::LayaDecider laya({.model = std::make_unique<StubLayaModel>(turn::EngineStatus::Ready,
                                                                   std::vector<std::pair<std::string, double>>{{"memory_save", 0.96}, {"none", 0.02}},
                                                                   0.9),
                          .fallback = nullptr,
                          .policy = bundle.policy(),
                          .confidence = bundle.confidenceCalibration(),
                          .now = bundle.nowCalibration()});
  turn::TurnFlow flow({.executor = world.executor,
                       .decider = &laya,
                       .text = &world.text,
                       .policies = turn::PolicySet(turn::DecisionPolicy{.act = bundle.policy().act,
                                                                        .ask = bundle.policy().ask,
                                                                        .margin = bundle.policy().margin,
                                                                        .nowMin = bundle.policy().now})});
  const turn::Outcome outcome = flow.run({.utterance = "recuerda que el codigo del porton es AB12CD",
                                          .offered = world.offered,
                                          .audience = world.audience,
                                          .context = world.context,
                                          .now = world.now,
                                          .previousAssistant = {}});
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().name == "memory.remember");
  CHECK(outcome.source == "laya, memory_save");
  CHECK(outcome.decidedTool == "memory.remember");
  CHECK(outcome.steps.size() == 1);
}
