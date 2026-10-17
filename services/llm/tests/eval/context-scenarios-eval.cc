#include "call-faithfulness-checks.hxx"
#include "context-checks.hxx"
#include "context-scenarios.hxx"
#include "eval-report.hxx"
#include "eval-tools.hxx"

#include <auth/capability-speech.hxx>
#include <auth/module-gate.hxx>
#include <config/config-service.hxx>
#include <feature/duress/services/duress-link.hxx>
#include <feature/duress/services/duress-screen.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/claim-check.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>
#include <feature/llm/services/turn/onnx-session.hxx>
#include <feature/llm/services/turn/tool-effects.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <llm/llm-service.hxx>
#include <llm/llm-speech.hxx>
#include <text/secret-phrase.hxx>
#include <text/sha256.hxx>
#include <text/text-norm.hxx>

#include <llama.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

constexpr int kSkipped = 77;
constexpr int kMaxTokens = 160;
constexpr int kDuressTurnMarker = -1;

struct Options
{
  std::vector<std::string> cases;
  std::string gates;
  std::string llmModel;
  std::string promptEs;
  std::string promptEn;
  std::string rolesEs;
  std::string rolesEn;
  std::string report;
  std::string trace;
  std::string table;
  std::string ttft;
  std::string blind;
  std::string filter;
  std::string regionalismMarkers;
  std::string decide;
  std::string decideBundle;
  std::string layaAgentConfig;
  std::string extract;
  std::string extractBundle;
  std::string variant{"production"};
  std::string label;
  std::vector<std::string> scenarios;
  std::vector<std::string> locales;
  std::string route;
  float temperature{0.3F};
  uint32_t seed{42};
  int limit{0};
  bool verbose{false};
  bool force{false};
  bool printConfig{false};
};

Options parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--cases" && hasValue)
      options.cases.emplace_back(argv[++i]);
    else if (arg == "--gates" && hasValue)
      options.gates = argv[++i];
    else if (arg == "--llm-model" && hasValue)
      options.llmModel = argv[++i];
    else if (arg == "--prompt-es" && hasValue)
      options.promptEs = argv[++i];
    else if (arg == "--prompt-en" && hasValue)
      options.promptEn = argv[++i];
    else if (arg == "--roles-es" && hasValue)
      options.rolesEs = argv[++i];
    else if (arg == "--roles-en" && hasValue)
      options.rolesEn = argv[++i];
    else if (arg == "--report" && hasValue)
      options.report = argv[++i];
    else if (arg == "--trace" && hasValue)
      options.trace = argv[++i];
    else if (arg == "--table" && hasValue)
      options.table = argv[++i];
    else if (arg == "--ttft" && hasValue)
      options.ttft = argv[++i];
    else if (arg == "--blind" && hasValue)
      options.blind = argv[++i];
    else if (arg == "--regionalism-markers" && hasValue)
      options.regionalismMarkers = argv[++i];
    else if (arg == "--filter" && hasValue)
      options.filter = argv[++i];
    else if (arg == "--decide" && hasValue)
      options.decide = argv[++i];
    else if (arg == "--decide-bundle" && hasValue)
      options.decideBundle = argv[++i];
    else if (arg == "--laya-agent-config" && hasValue)
      options.layaAgentConfig = argv[++i];
    else if (arg == "--extract" && hasValue)
      options.extract = argv[++i];
    else if (arg == "--extract-bundle" && hasValue)
      options.extractBundle = argv[++i];
    else if (arg == "--print-config")
      options.printConfig = true;
    else if (arg == "--variant" && hasValue)
      options.variant = argv[++i];
    else if (arg == "--label" && hasValue)
      options.label = argv[++i];
    else if (arg == "--scenario" && hasValue)
      options.scenarios.emplace_back(argv[++i]);
    else if (arg == "--locale" && hasValue)
      options.locales.emplace_back(argv[++i]);
    else if (arg == "--route" && hasValue)
      options.route = argv[++i];
    else if (arg == "--temperature" && hasValue)
      options.temperature = std::strtof(argv[++i], nullptr);
    else if (arg == "--seed" && hasValue)
      options.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (arg == "--limit" && hasValue)
      options.limit = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (arg == "--verbose")
      options.verbose = true;
    else if (arg == "--force")
      options.force = true;
  }
  return options;
}

struct FileBytes
{
  std::string bytes;
  std::string error;
};

FileBytes readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {.bytes = {}, .error = "cannot open " + path};
  return {.bytes = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), .error = {}};
}

std::string knownPathFor(const std::string& promptPath, std::string_view lang)
{
  return (std::filesystem::path(promptPath).parent_path() / ("call-known-" + std::string(lang) + ".txt")).string();
}

struct GatesState
{
  bool pinned{false};
  bool argsPinned{false};
  float temperature{0.0F};
  uint32_t seed{0};
  std::string error;
};

GatesState readGates(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return {.pinned = false, .argsPinned = false, .temperature = 0.0F, .seed = 0, .error = "cannot open " + path};
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  if (!Json::parseFromStream(builder, in, &root, &errors))
    return {.pinned = false, .argsPinned = false, .temperature = 0.0F, .seed = 0, .error = path + ": " + errors};
  const Json::Value& section = root["contextScenarios"];
  GatesState state{.pinned = section["metrics"].isObject(),
                   .argsPinned = false,
                   .temperature = 0.0F,
                   .seed = 0,
                   .error = {}};
  const Json::Value& args = section["pinnedArgs"];
  if (args.isObject() && args["temperature"].isNumeric() && args["seed"].isNumeric()) {
    state.argsPinned = true;
    state.temperature = static_cast<float>(args["temperature"].asDouble());
    state.seed = args["seed"].asUInt();
  }
  if (state.pinned && !state.argsPinned)
    state.error = path + " pins contextScenarios without the temperature and seed it was measured at";
  return state;
}

bool pinnedFor(const GatesState& gates, const Options& options)
{
  return gates.temperature == options.temperature && gates.seed == options.seed;
}

ModuleText textOf(const Json::Value& node)
{
  return {.es = node.get("es", "").asString(), .en = node.get("en", "").asString()};
}

ModuleIntroText introOf(const Json::Value& node)
{
  ModuleIntroText out;
  out.what = node.get("what", "").asString();
  for (const auto& example : node["examples"])
    out.examples.push_back(example.asString());
  return out;
}

ModuleFlags catalogFlags(const std::string& path)
{
  ModuleFlags flags;
  std::ifstream in(path);
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  if (!in || !Json::parseFromStream(builder, in, &root, &errors))
    return flags;
  const Json::Value& modules = root.isMember("modules") ? root["modules"] : root;
  for (const auto& node : modules) {
    ModuleFlag flag;
    flag.id = node["id"].asString();
    flag.enabled = true;
    flag.lifecycle = "active";
    flag.name = textOf(node["name"]);
    flag.summary = textOf(node["summary"]);
    flag.intro = {.es = introOf(node["intro"]["es"]), .en = introOf(node["intro"]["en"])};
    for (const auto& role : node["roles"])
      flag.roles.push_back(role.asString());
    flag.kind = node.get("kind", "").asString();
    flags.push_back(std::move(flag));
  }
  return flags;
}

std::string catalogPathFor(const std::string& casesPath)
{
  const std::filesystem::path derived = std::filesystem::path(casesPath).parent_path() / ".." / ".." / ".." / ".." /
                                        "settings" / "modules.json";
  if (std::filesystem::exists(derived))
    return derived.lexically_normal().string();
  const std::filesystem::path local = "services/settings/modules.json";
  if (std::filesystem::exists(local))
    return local.string();
  return derived.lexically_normal().string();
}

std::string withSpeaker(const std::string& persona, std::string_view lang, UserRole role, std::string_view name)
{
  const std::string line = role_access::speakerLine({.lang = lang, .role = role, .name = name, .voiceCall = true});
  if (line.empty())
    return persona;
  return persona + "\n" + line;
}

std::string tomlFloat(float value)
{
  std::string out = std::format("{}", value);
  if (out.find_first_of(".eEnN") == std::string::npos)
    out += ".0";
  return out;
}

std::string configKey(const Options& options)
{
  return std::format("eval:max_tokens={},temp={},seed={},variant={},cases={},decide={},extract={}",
                     kMaxTokens,
                     tomlFloat(options.temperature),
                     options.seed,
                     options.variant,
                     options.cases.size(),
                     std::filesystem::path(options.decideBundle).filename().string(),
                     std::filesystem::path(options.extractBundle).filename().string());
}

struct ScratchConfigInput
{
  const std::string& model;
  float temperature{0.0F};
  uint32_t seed{42};
};

std::string writeScratchConfig(const ScratchConfigInput& input)
{
  const std::string path = (std::filesystem::temp_directory_path() /
                            ("context-scenarios-eval-" + std::to_string(::getpid()) + ".toml"))
                               .string();
  std::ofstream out(path);
  out << "[llm]\nmodel_path = \"" << input.model << "\"\n"
      << "context_size = 8192\nmax_tokens = " << kMaxTokens << "\ntemperature = " << tomlFloat(input.temperature)
      << "\n"
      << "top_k = 20\ntop_p = 0.8\nmin_p = 0.0\npenalty_last_n = 64\npenalty_repeat = 1.10\n"
      << "seed = " << input.seed << "\nchat_template = \"chatml\"\ngpu_layers = -1\nn_batch = 1024\nn_ubatch = 512\n"
      << "kv_type = \"auto\"\nflash_attn = \"auto\"\nthreads = 4\nbatch_threads = 8\n"
      << "[drogon.app]\nnumber_of_threads = 2\n";
  return path;
}

class ArmableLink final : public DuressLink
{
public:
  [[nodiscard]] std::optional<std::string> phraseKey() const override
  {
    const std::scoped_lock lock(mutex_);
    ++asked_;
    return key_;
  }

  [[nodiscard]] DuressDelivery raise(const DuressRaise& raise) const override
  {
    const std::scoped_lock lock(mutex_);
    attempts_.push_back(raise);
    return DuressDelivery::Delivered;
  }

  void arm(std::string key)
  {
    const std::scoped_lock lock(mutex_);
    key_ = std::move(key);
  }

  void disarm()
  {
    const std::scoped_lock lock(mutex_);
    key_.reset();
  }

  [[nodiscard]] int asked() const
  {
    const std::scoped_lock lock(mutex_);
    return asked_;
  }

  [[nodiscard]] std::size_t raises() const
  {
    const std::scoped_lock lock(mutex_);
    return attempts_.size();
  }

private:
  mutable std::mutex mutex_;
  std::optional<std::string> key_;
  mutable int asked_{0};
  mutable std::vector<DuressRaise> attempts_;
};

bool disarmer(UserRole role)
{
  return role == UserRole::Owner || role == UserRole::Resident;
}

DuressScreenTiming duressTiming()
{
  return {.refresh = std::chrono::minutes(1),
          .retryBase = std::chrono::milliseconds(20),
          .retryMax = std::chrono::milliseconds(40),
          .memo = std::chrono::minutes(2),
          .queueLimit = 8};
}

struct DuressRig
{
  std::shared_ptr<ArmableLink> link;
  std::unique_ptr<DuressScreen> screen;
};

bool armDuress(const DuressRig& rig, const std::string& key)
{
  const int before = rig.link->asked();
  rig.link->arm(key);
  rig.screen->requestRefresh();
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (rig.link->asked() > before && rig.screen->idle())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return rig.link->asked() > before;
}

bool waitForRaises(const DuressRig& rig, std::size_t wanted)
{
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (rig.link->raises() >= wanted)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return rig.link->raises() >= wanted;
}

std::optional<DuressRig> openDuressRig()
{
  DuressRig rig{.link = std::make_shared<ArmableLink>(), .screen = nullptr};
  rig.screen = std::make_unique<DuressScreen>(DuressScreenDependencies{.link = rig.link,
                                                                       .eligible = disarmer,
                                                                       .outbox = {},
                                                                       .clock = {}},
                                              duressTiming());
  rig.screen->start();
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (rig.link->asked() > 0 && rig.screen->idle())
      return rig;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return rig;
}

struct Engines
{
  std::unique_ptr<turn::FirstOf> stack;
  std::unique_ptr<turn::LayaDecider> laya;
  std::unique_ptr<turn::GlinerExtractor> gliner;
  std::string active;
};

std::string bundlePin(const std::string& dir)
{
  if (dir.empty())
    return {};
  const FileBytes bytes = readFile((std::filesystem::path(dir) / "sha256").string());
  return bytes.error.empty() ? argus::hash::sha256Hex(bytes.bytes) : std::string{};
}

Engines installEngines(LlmController& controller, const Options& options)
{
  Engines engines;
  if (!options.decide.empty())
    ConfigService::setRuntimeString("decide.engine", options.decide);
  if (!options.decideBundle.empty())
    ConfigService::setRuntimeString("decide.laya.bundle_dir", options.decideBundle);
  if (!options.extract.empty())
    ConfigService::setRuntimeString("extract.engine", options.extract);
  if (!options.extractBundle.empty())
    ConfigService::setRuntimeString("extract.gliner.bundle_dir", options.extractBundle);
  if (!options.layaAgentConfig.empty())
    ConfigService::setRuntimeString("decide.laya.agent_config", options.layaAgentConfig);
  if (options.decide == "laya") {
    const turn::BundleLoader bundle({.dir = options.decideBundle, .pin = bundlePin(options.decideBundle)});
    if (bundle.valid()) {
      engines.laya = std::make_unique<turn::LayaDecider>(turn::LayaDeciderInput{
          .model = turn::openLayaModel(
              {.bundle = bundle,
               .options = {},
               .decode = turn::layaDecodeFromAgentConfig(ConfigService::getString("decide.laya.agent_config"))
                             .value_or(bundle.decode())}),
          .fallback = &controller.adapter().routerDecider(),
          .policy = bundle.policy(),
          .confidence = bundle.confidenceCalibration(),
          .now = bundle.nowCalibration()});
      engines.stack = std::make_unique<turn::FirstOf>(
          std::vector<const turn::Decider*>{&controller.adapter().ruleDecider(), engines.laya.get()});
      controller.adapter().flow().useDecider(*engines.stack);
      turn::PolicySet policies;
      policies.set(std::string(turn::kDeciderIds[2]),
                   turn::DecisionPolicy{.act = bundle.policy().act,
                                        .ask = bundle.policy().ask,
                                        .margin = bundle.policy().margin,
                                        .nowMin = bundle.policy().now});
      controller.adapter().flow().usePolicies(std::move(policies));
      engines.active = "laya";
    }
    else {
      std::cout << std::format("[WARN] the Laya bundle is off: {}\n", bundle.error());
    }
  }
  if (options.extract == "gliner") {
    const turn::BundleLoader bundle({.dir = options.extractBundle,
                                     .pin = bundlePin(options.extractBundle),
                                     .kind = turn::BundleKind::Extractor});
    if (bundle.valid()) {
      engines.gliner = std::make_unique<turn::GlinerExtractor>(
          turn::GlinerExtractorInput{.model = turn::openGlinerModel(bundle, {}), .fallback = nullptr,
                                     .thresholds = bundle.thresholds()});
      controller.adapter().flow().useText(*engines.gliner);
      engines.active += (engines.active.empty() ? "" : "+");
      engines.active += "gliner";
    }
    else {
      std::cout << std::format("[WARN] the GLiNER bundle is off: {}\n", bundle.error());
    }
  }
  if (engines.active.empty())
    engines.active = "rules+router";
  return engines;
}

std::vector<std::string> offeredFacts(const context_eval::ScenarioCase& item)
{
  std::vector<std::string> out;
  for (const context_eval::ScenarioFact& fact : item.facts)
    out.push_back(fact.text);
  return out;
}

const std::vector<context_eval::ScenarioFact>& factsFor(const context_eval::ScenarioCase& item,
                                                         std::string_view variant)
{
  return variant == "oracle" ? item.oracle : item.facts;
}

struct TurnStateInput
{
  const std::string& utterance;
  const std::vector<tools::ToolHandle>& offered;
  const std::vector<eval::RecordedCall>& executed;
  const std::string& lang;
};

TurnState stateFor(const TurnStateInput& input)
{
  const bool appOffered = std::ranges::any_of(
      input.offered, [](const tools::ToolHandle& tool) { return isAppTool(tool->spec.name); });
  const bool appAsked = appOffered && asksForAppAction(input.utterance);
  TurnState state{.asked = appAsked || reply_claims::asksForAction(input.utterance),
                  .appAsked = appAsked,
                  .wrote = false,
                  .opened = false,
                  .called = false,
                  .lang = input.lang};
  for (const auto& call : input.executed) {
    const tools::ToolHandle tool = ToolRegistry::instance().find(call.tool);
    if (!tool)
      continue;
    const bool performed = !tool->spec.annotations.readOnly || isAppTool(call.tool);
    state.wrote = state.wrote || (performed && !turn::isReadOnlyTool(call.tool));
    state.opened = state.opened || (performed && turn::isReadOnlyTool(call.tool) && isAppTool(call.tool));
  }
  return state;
}

struct TurnRun
{
  std::string reply;
  std::string rawReply;
  std::string contextBlock;
  std::string act;
  std::string speech;
  std::string guardVerdict;
  std::vector<eval::RecordedCall> executed;
  context_checks::ContextVerdict verdict;
  int64_t ms{0};
  int32_t promptTokens{0};
  int32_t decodedTokens{0};
  bool duressHeld{false};
};

struct CaseRun
{
  context_eval::ScenarioCase item;
  std::string staticPrefix;
  std::vector<TurnRun> turns;
};

int64_t userIdFor(UserRole role)
{
  switch (role) {
    case UserRole::Owner:
      return 7;
    case UserRole::Resident:
      return 8;
    case UserRole::Guard:
      return 9;
    case UserRole::Guest:
      return 10;
    default:
      return 7;
  }
}

std::string poolFor(const context_eval::ScenarioCase& item, std::string_view variant)
{
  std::string pool = item.person;
  if (item.call)
    pool += " " + item.call->opening;
  for (const context_eval::ScenarioTurn& turn : item.turns)
    pool += " " + turn.user;
  for (const context_eval::ScenarioFact& fact : item.facts)
    pool += " " + fact.text;
  for (const context_eval::ScenarioFact& fact : factsFor(item, variant))
    pool += " " + fact.text;
  return pool;
}

struct CaseInput
{
  LlmController& controller;
  const context_eval::ScenarioCase& item;
  const eval::StubInput& stubs;
  const std::vector<std::string>& markers;
  const std::string& prompt;
  const std::string& known;
  const std::string& roles;
  std::string variant;
  float temperature{0.0F};
};

CaseRun runCase(const CaseInput& input)
{
  CaseRun run{.item = input.item, .staticPrefix = {}, .turns = {}};
  const UserRole role = userRoleFromString(input.item.role);
  ModuleFlags flags = moduleGate().known();
  for (ModuleFlag& flag : flags)
    if (std::ranges::find(input.item.modulesOff, flag.id) != input.item.modulesOff.end())
      flag.enabled = false;
  std::ignore = moduleGate().apply(flags);
  const std::vector<tools::ToolHandle> offered =
      input.controller.adapter().executor().offered({.role = role, .modules = moduleGate().snapshot()});
  run.staticPrefix = call_checks::staticPrompt({.roles = std::nullopt,
                                                .rolesDefault = input.roles,
                                                .prompt = withSpeaker(input.prompt, input.item.lang, role, input.item.name),
                                                .person = input.item.person,
                                                .known = input.known,
                                                .facts = {}});
  std::vector<ChatMessage> history;
  history.push_back({.role = "system", .content = run.staticPrefix});
  if (input.item.call)
    history.push_back({.role = "assistant", .content = input.item.call->opening});
  const std::string pool = poolFor(input.item, input.variant);
  bool firstTurn = true;
  for (std::size_t index = 0; index < input.item.turns.size(); ++index) {
    const context_eval::ScenarioTurn& turn = input.item.turns[index];
    const bool duressTurn = input.item.duress && static_cast<int>(index) == input.item.duress->turn;
    input.stubs.recorder->clear();
    ChatRequest request;
    request.messages = history;
    request.messages.push_back({.role = "user", .content = turn.user});
    request.contextFacts = {};
    for (const context_eval::ScenarioFact& fact : factsFor(input.item, input.variant))
      request.contextFacts.push_back({.facet = fact.facet, .text = fact.text});
    request.maxTokens = kMaxTokens;
    request.temperature = input.temperature;
    request.resetContext = firstTurn;
    request.userId = userIdFor(role);
    request.role = role;
    request.lang = input.item.lang;
    request.clientActions = true;
    request.sessionId = "context-scenarios-" + input.item.id;
    const auto started = std::chrono::steady_clock::now();
    const LlmChatOutcome outcome = input.controller.chatSync(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const LlmPrefillStats prefill = input.controller.service().lastPrefillStats();
    TurnRun recorded;
    recorded.reply = outcome.text;
    recorded.rawReply = outcome.rawReply;
    recorded.contextBlock = outcome.contextBlock;
    recorded.act = outcome.act;
    recorded.speech = outcome.speech;
    recorded.guardVerdict = outcome.guardVerdict;
    recorded.executed = input.stubs.recorder->executed();
    recorded.ms = elapsed.count();
    recorded.promptTokens = prefill.promptTokens;
    recorded.decodedTokens = prefill.decodedTokens;
    recorded.duressHeld = duressTurn && outcome.text.empty() && outcome.speech == llm_speech::kUnheard;
    const TurnState state =
        stateFor({.utterance = turn.user, .offered = offered, .executed = recorded.executed, .lang = input.item.lang});
    const bool productionClaim = claimedWithoutTool(recorded.reply, state);
    const bool callClaim = reply_claims::claimsCall({.text = recorded.reply, .lang = input.item.lang});
    recorded.verdict = context_checks::scoreContextTurn(
        {.turn = turn,
         .item = input.item,
         .reply = recorded.reply,
         .pool = pool,
         .emittedAct = recorded.act,
         .expectedAct = turn.act,
         .firstTurn = firstTurn,
         .duressTurn = duressTurn,
         .duressHeld = recorded.duressHeld,
         .productionClaim = productionClaim,
         .callClaim = callClaim});
    recorded.verdict.regionalism =
        input.item.locale == "es-PE" && call_checks::speaksRegionalism(recorded.reply, input.markers);
    if (!duressTurn)
      history.push_back({.role = "assistant", .content = outcome.text});
    run.turns.push_back(std::move(recorded));
    firstTurn = false;
  }
  return run;
}

std::vector<context_eval::ScenarioCase> selected(const std::vector<context_eval::ScenarioCase>& cases,
                                                  const Options& options)
{
  std::vector<context_eval::ScenarioCase> out;
  for (const context_eval::ScenarioCase& item : cases) {
    if (!options.filter.empty() && item.id.find(options.filter) == std::string::npos)
      continue;
    if (!options.scenarios.empty() && std::ranges::find(options.scenarios, item.scenario) == options.scenarios.end())
      continue;
    if (!options.locales.empty() && std::ranges::find(options.locales, item.locale) == options.locales.end())
      continue;
    if (!options.route.empty() && item.route != options.route)
      continue;
    out.push_back(item);
  }
  if (options.limit > 0 && out.size() > static_cast<size_t>(options.limit))
    out.resize(static_cast<size_t>(options.limit));
  return out;
}

Json::Value turnRow(const CaseRun& run, std::size_t index, const Options& options)
{
  const TurnRun& turn = run.turns[index];
  Json::Value row(Json::objectValue);
  row["id"] = run.item.id;
  row["scenario"] = run.item.scenario;
  row["axis"] = run.item.axis;
  row["locale"] = run.item.locale;
  row["lang"] = run.item.lang;
  row["role"] = run.item.role;
  row["route"] = run.item.route;
  row["context"] = options.variant;
  row["model"] = std::filesystem::path(options.llmModel).filename().string();
  row["label"] = options.label;
  row["turn"] = static_cast<int>(index);
  row["act"] = turn.act;
  row["speech"] = turn.speech;
  row["verdict"] = context_checks::violationsOf(turn.verdict);
  row["reply"] = turn.reply;
  row["rawReply"] = turn.rawReply;
  row["guardVerdict"] = turn.guardVerdict;
  row["contextBlock"] = turn.contextBlock;
  row["ms"] = static_cast<Json::Value::Int64>(turn.ms);
  row["promptTokens"] = turn.promptTokens;
  row["decodedTokens"] = turn.decodedTokens;
  row["duressHeld"] = turn.duressHeld;
  row["executed"] = Json::Value(Json::arrayValue);
  for (const auto& call : turn.executed)
    row["executed"].append(call.tool);
  return row;
}

bool writeTrace(const std::string& path, const std::vector<CaseRun>& runs, const Options& options)
{
  std::ofstream out(path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  for (const CaseRun& run : runs)
    for (std::size_t index = 0; index < run.turns.size(); ++index)
      out << Json::writeString(builder, turnRow(run, index, options)) << "\n";
  return static_cast<bool>(out);
}

bool writeBlind(const std::string& dir, const std::vector<CaseRun>& runs, const Options& options)
{
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  const std::string stem = std::filesystem::path(options.llmModel).filename().string() + "." + options.variant;
  std::ofstream sample(std::filesystem::path(dir) / (stem + ".sample.jsonl"));
  std::ofstream key(std::filesystem::path(dir) / (stem + ".key.jsonl"));
  if (!sample || !key)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  for (const CaseRun& run : runs) {
    for (std::size_t index = 0; index < run.turns.size(); ++index) {
      if (run.turns[index].reply.empty())
        continue;
      Json::Value text(Json::objectValue);
      text["text"] = run.turns[index].reply;
      sample << Json::writeString(builder, text) << "\n";
      Json::Value row(Json::objectValue);
      row["id"] = run.item.id;
      row["scenario"] = run.item.scenario;
      row["axis"] = run.item.axis;
      row["locale"] = run.item.locale;
      row["route"] = run.item.route;
      row["context"] = options.variant;
      row["model"] = std::filesystem::path(options.llmModel).filename().string();
      row["label"] = options.label;
      row["turn"] = static_cast<int>(index);
      key << Json::writeString(builder, row) << "\n";
    }
  }
  return static_cast<bool>(sample) && static_cast<bool>(key);
}

bool writeTable(const std::string& path, const std::vector<context_checks::ScenarioRow>& rows)
{
  std::ofstream out(path);
  if (!out)
    return false;
  Json::Value root(Json::arrayValue);
  for (const context_checks::ScenarioRow& row : rows) {
    Json::Value entry(Json::objectValue);
    entry["key"] = row.key;
    for (const context_checks::TableColumn& column : row.columns)
      entry["metrics"][column.name] = column.value;
    root.append(entry);
  }
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  out << Json::writeString(builder, root) << "\n";
  return static_cast<bool>(out);
}

struct TtftInput
{
  LlmController& controller;
  const std::vector<context_eval::ScenarioCase>& cases;
  std::string variant;
  std::string path;
  float temperature{0.0F};
};

bool runTtft(const TtftInput& input)
{
  std::ofstream out(input.path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  for (const context_eval::ScenarioCase& item : input.cases) {
    std::vector<ChatMessage> history;
    history.push_back({.role = "system", .content = "persona"});
    int turn = 0;
    for (const context_eval::ScenarioTurn& script : item.turns) {
      history.push_back({.role = "user", .content = script.user});
      ChatRequest request;
      request.messages = history;
      request.contextFacts = {};
      for (const context_eval::ScenarioFact& fact : factsFor(item, input.variant))
        request.contextFacts.push_back({.facet = fact.facet, .text = fact.text});
      request.maxTokens = kMaxTokens;
      request.temperature = input.temperature;
      request.resetContext = turn == 0;
      request.userId = 7;
      request.role = userRoleFromString(item.role);
      request.lang = item.lang;
      request.clientActions = true;
      request.sessionId = "context-scenarios-" + item.id;
      LlmPrefillStats stats;
      std::chrono::steady_clock::time_point first{};
      std::string reply;
      const auto started = std::chrono::steady_clock::now();
      input.controller.chatStreamSync({.request = request,
                                       .onToken = [&](const std::string& token, bool done) {
                                         if (done)
                                           return;
                                         if (first == std::chrono::steady_clock::time_point{})
                                           first = std::chrono::steady_clock::now();
                                         reply += token;
                                       },
                                       .stats = &stats,
                                       .cancellation = {},
                                       .onAction = {}});
      const int64_t firstMs = first == std::chrono::steady_clock::time_point{}
                                  ? -1
                                  : std::chrono::duration_cast<std::chrono::milliseconds>(first - started).count();
      Json::Value row(Json::objectValue);
      row["id"] = item.id;
      row["locale"] = item.locale;
      row["route"] = item.route;
      row["context"] = input.variant;
      row["turn"] = turn;
      row["firstTokenMs"] = static_cast<Json::Value::Int64>(firstMs);
      row["promptTokens"] = stats.promptTokens;
      row["decodedTokens"] = stats.decodedTokens;
      out << Json::writeString(builder, row) << "\n";
      history.push_back({.role = "assistant", .content = reply});
      ++turn;
    }
  }
  return static_cast<bool>(out);
}

std::vector<context_checks::ScoredCase> scoredOf(const std::vector<CaseRun>& runs)
{
  std::vector<context_checks::ScoredCase> out;
  for (const CaseRun& run : runs) {
    context_checks::ScoredCase scored{.item = run.item, .turns = {}};
    for (const TurnRun& turn : run.turns) {
      context_checks::ScoredTurn entry;
      entry.verdict = turn.verdict;
      entry.act = turn.act;
      entry.ms = turn.ms;
      entry.promptTokens = turn.promptTokens;
      entry.decodedTokens = turn.decodedTokens;
      entry.duressHeld = turn.duressHeld;
      scored.turns.push_back(std::move(entry));
    }
    out.push_back(std::move(scored));
  }
  return out;
}

struct FinishInput
{
  const Options& options;
  const GatesState& gates;
  const std::vector<CaseRun>& runs;
};

int finish(const FinishInput& input)
{
  const std::vector<context_checks::ScoredCase> scored = scoredOf(input.runs);
  const eval::Metrics metrics = context_checks::contextMetrics(scored);
  eval::Verdict verdict;
  {
    const eval::LoadedGates gates = eval::loadGates(input.options.gates, "contextScenarios");
    if (!gates.error.empty()) {
      std::cout << "[ERROR] " << gates.error << "\n";
      return 1;
    }
    verdict = eval::check(gates.gates, metrics);
  }
  std::cout << std::format("context scenarios: {} cases, {} turns, context {}\n",
                           input.runs.size(),
                           metrics.at("turns"),
                           input.options.variant);
  eval::printMetrics(metrics);
  context_checks::printScenarioTable(context_checks::scenarioTable(scored, "scenario"), "scenario x metric:");
  context_checks::printScenarioTable(context_checks::scenarioTable(scored, "axis"), "axis x metric:");
  if (!input.options.table.empty() && !writeTable(input.options.table, context_checks::scenarioTable(scored, "scenario"))) {
    std::cout << "[ERROR] cannot write " << input.options.table << "\n";
    return 1;
  }
  if (!input.options.report.empty() &&
      !eval::writeReport(input.options.report,
                         {.section = "contextScenarios", .metrics = metrics, .verdict = verdict,
                          .pinned = input.gates.pinned})) {
    std::cout << "[ERROR] cannot write " << input.options.report << "\n";
    return 1;
  }
  if (!verdict.passed()) {
    std::cout << "GATE FAILED\n";
    for (const auto& failure : verdict.failures)
      std::cout << "  " << failure << "\n";
    return 1;
  }
  std::cout << "GATE PASSED\n";
  return 0;
}

}

int main(int argc, char** argv)
{
  const Options options = parseOptions(argc, argv);
  if (options.printConfig) {
    std::cout << configKey(options) << "\n";
    return 0;
  }
  if (options.cases.empty()) {
    std::cout << "[ERROR] no scenario pack given; pass --cases <file> at least once\n";
    return 1;
  }
  if (options.variant != "production" && options.variant != "oracle") {
    std::cout << "[ERROR] unknown context variant " << options.variant << "; use production or oracle\n";
    return 1;
  }
#ifndef NDEBUG
  if (!options.force) {
    std::cout << "[SKIPPED] a debug build decodes about twenty times slower; run the prod build or pass --force\n";
    return kSkipped;
  }
#endif
  const GatesState gates = readGates(options.gates);
  if (!gates.error.empty()) {
    std::cout << "[ERROR] " << gates.error << "\n";
    return 1;
  }
  if (gates.pinned && !pinnedFor(gates, options)) {
    std::cout << std::format("[SKIPPED] gates pinned for different args: {} pinned at --temperature {} --seed {}, "
                             "this run --temperature {} --seed {}\n",
                             options.gates,
                             gates.temperature,
                             gates.seed,
                             options.temperature,
                             options.seed);
    return kSkipped;
  }
  if (!std::filesystem::exists(options.llmModel)) {
    std::cout << "[SKIPPED] no LLM weights at " << options.llmModel << "; the context gate did not run\n";
    return kSkipped;
  }
  const FileBytes spanish = readFile(options.promptEs);
  if (!spanish.error.empty()) {
    std::cout << "[ERROR] " << spanish.error << "\n";
    return 1;
  }
  const FileBytes english = readFile(options.promptEn);
  if (!english.error.empty()) {
    std::cout << "[ERROR] " << english.error << "\n";
    return 1;
  }
  const FileBytes knownEs = readFile(knownPathFor(options.promptEs, "es"));
  if (!knownEs.error.empty()) {
    std::cout << "[ERROR] " << knownEs.error << "\n";
    return 1;
  }
  const FileBytes knownEn = readFile(knownPathFor(options.promptEn, "en"));
  if (!knownEn.error.empty()) {
    std::cout << "[ERROR] " << knownEn.error << "\n";
    return 1;
  }
  std::string rolesEs;
  if (!options.rolesEs.empty()) {
    const FileBytes roles = readFile(options.rolesEs);
    if (!roles.error.empty()) {
      std::cout << "[ERROR] " << roles.error << "\n";
      return 1;
    }
    rolesEs = roles.bytes;
  }
  std::string rolesEn;
  if (!options.rolesEn.empty()) {
    const FileBytes roles = readFile(options.rolesEn);
    if (!roles.error.empty()) {
      std::cout << "[ERROR] " << roles.error << "\n";
      return 1;
    }
    rolesEn = roles.bytes;
  }
  const std::vector<std::string> markers = options.regionalismMarkers.empty()
                                               ? std::vector<std::string>{}
                                               : call_checks::markersFromFile(options.regionalismMarkers);
  if (!options.regionalismMarkers.empty() && markers.empty()) {
    std::cout << "[ERROR] no regionalism markers in " << options.regionalismMarkers << "\n";
    return 1;
  }
  const context_eval::LoadedScenarios loaded = context_eval::loadScenarioPack(options.cases);
  if (!loaded.error.empty()) {
    std::cout << "[ERROR] " << loaded.error << "\n";
    return 1;
  }
  const std::string catalogPath = catalogPathFor(options.cases.front());
  const ModuleFlags catalog = catalogFlags(catalogPath);
  if (catalog.empty()) {
    std::cout << "[ERROR] no module catalog at " << catalogPath << "\n";
    return 1;
  }
  const std::string scratch =
      writeScratchConfig({.model = options.llmModel, .temperature = options.temperature, .seed = options.seed});
  ConfigService::load(scratch);
  std::filesystem::remove(scratch);
  llama_backend_init();
  LlmController controller;
  controller.initEngine();
  if (!controller.isEngineLoaded()) {
    std::cout << "[ERROR] the LLM did not load from " << options.llmModel << "\n";
    llama_backend_free();
    return 1;
  }
  const eval::StubInput stubs{.recorder = std::make_shared<eval::Recorder>(),
                              .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()};
  for (auto& descriptor : eval::stubTools(stubs))
    ToolRegistry::instance().registerTool(std::move(descriptor));
  std::ignore = moduleGate().apply(catalog);
  const Engines engines = installEngines(controller, options);
  std::cout << std::format("pipeline: {} context={} model={}\n",
                           engines.active,
                           options.variant,
                           std::filesystem::path(options.llmModel).filename().string());
  if ((options.decide == "laya" && engines.laya == nullptr) ||
      (options.extract == "gliner" && engines.gliner == nullptr)) {
    std::cout << "[REFUSED] an engine the run asked for is not up; no gate is reported\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }
  const std::vector<context_eval::ScenarioCase> cases = selected(loaded.cases, options);
  std::cout << std::format("context scenarios: {} cases of {}\n", cases.size(), loaded.cases.size());
  const bool needsDuress = std::ranges::any_of(cases, [](const context_eval::ScenarioCase& item) {
    return item.duress.has_value();
  });
  std::optional<DuressRig> duress;
  if (needsDuress) {
    duress = openDuressRig();
    controller.useScreen(*duress->screen);
  }
  std::vector<CaseRun> runs;
  runs.reserve(cases.size());
  for (const auto& item : cases) {
    const bool englishCase = item.lang == "en";
    if (item.duress) {
      const auto sealed = secret_phrase::seal({.phrase = item.duress->phrase,
                                               .salt = "context-scenarios-duress",
                                               .iterations = secret_phrase::kMinIterations});
      if (!sealed || !armDuress(*duress, secret_phrase::encode(*sealed))) {
        std::cout << "[ERROR] the duress screen did not take the phrase of " << item.id << "\n";
        controller.shutdownEngine();
        llama_backend_free();
        return 1;
      }
    }
    else if (duress) {
      duress->link->disarm();
    }
    const std::size_t raisesBefore = duress ? duress->link->raises() : 0;
    runs.push_back(runCase({.controller = controller,
                            .item = item,
                            .stubs = stubs,
                            .markers = markers,
                            .prompt = englishCase ? english.bytes : spanish.bytes,
                            .known = englishCase ? knownEn.bytes : knownEs.bytes,
                            .roles = englishCase ? rolesEn : rolesEs,
                            .variant = options.variant,
                            .temperature = options.temperature}));
    if (item.duress && !waitForRaises(*duress, raisesBefore + 1)) {
      std::cout << "[ERROR] the duress path did not fire for " << item.id << "\n";
      controller.shutdownEngine();
      llama_backend_free();
      return 1;
    }
    if (options.verbose) {
      const CaseRun& run = runs.back();
      std::cout << std::format("{} [{} {}] {} turns\n", run.item.id, run.item.locale, run.item.axis, run.turns.size());
      for (std::size_t index = 0; index < run.turns.size(); ++index)
        std::cout << std::format("  {}ms | {} | {}\n",
                                 run.turns[index].ms,
                                 context_checks::violationsOf(run.turns[index].verdict),
                                 run.turns[index].reply);
    }
  }
  if (duress)
    duress->screen->requestStop();
  std::ignore = moduleGate().apply(catalog);
  std::cout << std::format("consumption: process_rss_mb={} pipeline={}\n",
                           static_cast<double>(turn::residentBytes()) / (1024.0 * 1024.0),
                           engines.active);
  if (!options.trace.empty() && !writeTrace(options.trace, runs, options)) {
    std::cout << "[ERROR] cannot write " << options.trace << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }
  if (!options.blind.empty() && !writeBlind(options.blind, runs, options)) {
    std::cout << "[ERROR] cannot write the blind sample under " << options.blind << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }
  if (!options.ttft.empty() &&
      !runTtft({.controller = controller,
                .cases = cases,
                .variant = options.variant,
                .path = options.ttft,
                .temperature = options.temperature})) {
    std::cout << "[ERROR] cannot write " << options.ttft << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }
  controller.shutdownEngine();
  llama_backend_free();
  return finish({.options = options, .gates = gates, .runs = runs});
}
