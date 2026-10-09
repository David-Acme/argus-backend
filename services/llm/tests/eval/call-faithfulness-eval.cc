#include "call-faithfulness-checks.hxx"
#include "eval-report.hxx"
#include "eval-score.hxx"
#include "eval-tools.hxx"

#include <auth/module-gate.hxx>
#include <config/config-service.hxx>
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
#include <feature/llm/services/turn/speech-acts.hxx>
#include <feature/llm/services/turn/speech-guard.hxx>
#include <feature/llm/services/turn/speech-render.hxx>
#include <feature/llm/services/turn/tool-effects.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <llm/llm-service.hxx>
#include <mcp/confirmation.hxx>
#include <text/iso-time.hxx>
#include <text/sha256.hxx>
#include <text/text-norm.hxx>

#include <llama.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr int kSkipped = 77;
constexpr int64_t kEvalUser = 7;
constexpr int kMaxTokens = 160;
constexpr int kMaxSentences = 2;

struct Options
{
  std::string cases;
  std::string gates;
  std::string llmModel;
  std::string promptEs;
  std::string promptEn;
  std::string rolesEs;
  std::string rolesEn;
  std::string report;
  std::string dump;
  std::string ttft;
  std::string filter;
  std::string decide;
  std::string decideBundle;
  std::string layaAgentConfig;
  std::string extract;
  std::string extractBundle;
  std::string rejectSample;
  std::string regionalismMarkers;
  std::vector<std::string> dropFacets;
  float temperature{0.0F};
  uint32_t seed{42};
  int limit{0};
  bool verbose{false};
  bool force{false};
  bool legacyContext{false};
  bool renderActs{false};
  int64_t now{0};
};

Options parseOptions(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == "--cases" && hasValue)
      options.cases = argv[++i];
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
    else if (arg == "--dump" && hasValue)
      options.dump = argv[++i];
    else if (arg == "--ttft" && hasValue)
      options.ttft = argv[++i];
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
    else if (arg == "--reject-sample" && hasValue)
      options.rejectSample = argv[++i];
    else if (arg == "--regionalism-markers" && hasValue)
      options.regionalismMarkers = argv[++i];
    else if (arg == "--render-acts")
      options.renderActs = true;
    else if (arg == "--drop-facets" && hasValue)
      options.dropFacets.push_back(argv[++i]);
    else if (arg == "--temperature" && hasValue)
      options.temperature = std::strtof(argv[++i], nullptr);
    else if (arg == "--seed" && hasValue)
      options.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (arg == "--limit" && hasValue)
      options.limit = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (arg == "--now" && hasValue)
      options.now = static_cast<int64_t>(std::strtoll(argv[++i], nullptr, 10));
    else if (arg == "--verbose")
      options.verbose = true;
    else if (arg == "--force")
      options.force = true;
    else if (arg == "--legacy-context")
      options.legacyContext = true;
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

struct CallCase
{
  std::string id;
  std::string lang;
  std::vector<ContextFact> facts;
  std::vector<std::string> notes;
  std::vector<std::string> script;
  std::vector<std::string> noEcho;
  std::vector<std::string> expectAny;
  std::vector<std::string> expectFacts;
  std::vector<std::string> forbidFacts;
  std::vector<std::string> acts;
  std::string person;
  std::optional<std::string> roles;
  bool asksClock{false};
  bool asksCamera{false};
  bool greetsUser{false};
  bool nameUnknown{false};
};

struct LoadedCallCases
{
  std::vector<CallCase> cases;
  std::string error;
};

std::vector<std::string> stringList(const Json::Value& node)
{
  std::vector<std::string> out;
  for (const auto& item : node)
    out.push_back(item.asString());
  return out;
}

LoadedCallCases loadCallCases(const std::string& path)
{
  LoadedCallCases loaded;
  std::ifstream in(path);
  if (!in) {
    loaded.error = "cannot open " + path;
    return loaded;
  }
  std::string line;
  size_t lineNumber = 0;
  Json::CharReaderBuilder builder;
  while (std::getline(in, line)) {
    ++lineNumber;
    if (line.empty())
      continue;
    Json::Value node;
    std::string errors;
    std::istringstream stream(line);
    if (!Json::parseFromStream(builder, stream, &node, &errors)) {
      loaded.error = std::format("{}:{}: {}", path, lineNumber, errors);
      loaded.cases.clear();
      return loaded;
    }
    CallCase item;
    item.id = node["id"].asString();
    item.lang = node.get("lang", "es").asString();
    for (const auto& fact : node["facts"]) {
      if (fact.isObject())
        item.facts.push_back({.facet = fact.get("facet", "").asString(), .text = fact.get("text", "").asString()});
    }
    for (const auto& fact : item.facts)
      item.notes.push_back(fact.text);
    item.script = stringList(node["script"]);
    item.noEcho = stringList(node["noEcho"]);
    item.expectAny = stringList(node["expectAny"]);
    item.expectFacts = stringList(node["expectFacts"]);
    item.forbidFacts = stringList(node["forbidFacts"]);
    item.acts = stringList(node["acts"]);
    item.person = node.get("person", "").asString();
    if (node.isMember("roles"))
      item.roles = node["roles"].asString();
    item.asksClock = node.get("asksClock", false).asBool();
    item.asksCamera = node.get("asksCamera", false).asBool();
    item.greetsUser = node.get("greetsUser", false).asBool();
    item.nameUnknown = node.get("nameUnknown", false).asBool();
    loaded.cases.push_back(std::move(item));
  }
  return loaded;
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
  const Json::Value& section = root["callFaithfulness"];
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
    state.error = path + " pins callFaithfulness without the temperature and seed it was measured at";
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

std::string tomlFloat(float value)
{
  std::string out = std::format("{}", value);
  if (out.find_first_of(".eEnN") == std::string::npos)
    out += ".0";
  return out;
}

struct ScratchConfigInput
{
  const std::string& model;
  float temperature{0.0F};
  uint32_t seed{42};
};

std::string writeScratchConfig(const ScratchConfigInput& input)
{
  const std::string path =
      (std::filesystem::temp_directory_path() / ("call-faithfulness-eval-" + std::to_string(::getpid()) + ".toml")).string();
  std::ofstream out(path);
  out << "[llm]\nmodel_path = \"" << input.model << "\"\n"
      << "context_size = 8192\nmax_tokens = " << kMaxTokens << "\ntemperature = " << tomlFloat(input.temperature) << "\n"
      << "top_k = 20\ntop_p = 0.8\nmin_p = 0.0\npenalty_last_n = 64\npenalty_repeat = 1.10\n"
      << "seed = " << input.seed << "\nchat_template = \"chatml\"\ngpu_layers = -1\nn_batch = 1024\nn_ubatch = 512\n"
      << "kv_type = \"auto\"\nflash_attn = \"auto\"\nthreads = 4\nbatch_threads = 8\n"
      << "[drogon.app]\nnumber_of_threads = 2\n";
  return path;
}

constexpr std::array<std::string_view, 11> kEnglishTokens{"the", "you", "your", "and", "with", "what", "this", "that", "have", "how", "can"};
constexpr std::array<std::string_view, 9> kSpanishTokens{"el", "la", "los", "las", "de", "que", "y", "es", "está"};
constexpr std::array<std::string_view, 7> kRoleLabels{"Usuario:", "Tú:", "User:", "You:", "Argus:", "Sistema:", "System:"};

bool namesATime(const std::string& reply)
{
  for (size_t at = 0; at < reply.size(); ++at) {
    if (std::isdigit(static_cast<unsigned char>(reply[at])) == 0)
      continue;
    size_t cursor = at + 1;
    if (cursor < reply.size() && std::isdigit(static_cast<unsigned char>(reply[cursor])) != 0)
      ++cursor;
    if (cursor + 2 < reply.size() && reply[cursor] == ':' &&
        std::isdigit(static_cast<unsigned char>(reply[cursor + 1])) != 0 &&
        std::isdigit(static_cast<unsigned char>(reply[cursor + 2])) != 0)
      return true;
  }
  return false;
}

bool speaksForeignTokens(const std::string& reply, std::string_view lang)
{
  const std::vector<std::string> words = call_checks::wordsOf(reply);
  const std::span<const std::string_view> banned =
      lang == "en" ? std::span<const std::string_view>(kSpanishTokens) : std::span<const std::string_view>(kEnglishTokens);
  return std::ranges::any_of(banned, [&words](std::string_view token) { return call_checks::hasWord(words, token); });
}

std::vector<std::string> loadRegionalismMarkers(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    return {};
  std::vector<std::string> markers;
  std::string line;
  while (std::getline(in, line)) {
    const std::string folded = text_norm::folded(line);
    const std::size_t first = folded.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    const std::size_t last = folded.find_last_not_of(" \t\r\n");
    markers.push_back(folded.substr(first, last - first + 1));
  }
  return markers;
}

bool containsPhrase(const std::vector<std::string>& words, const std::vector<std::string>& needle)
{
  if (needle.empty() || needle.size() > words.size())
    return false;
  for (std::size_t at = 0; at + needle.size() <= words.size(); ++at) {
    bool hit = true;
    for (std::size_t index = 0; index < needle.size() && hit; ++index)
      hit = words.at(at + index) == needle.at(index);
    if (hit)
      return true;
  }
  return false;
}

bool speaksRegionalism(const std::string& reply, const std::vector<std::string>& markers)
{
  if (markers.empty())
    return false;
  const std::vector<std::string> words = call_checks::wordsOf(reply);
  return std::ranges::any_of(markers, [&words](const std::string& marker) {
    return containsPhrase(words, call_checks::wordsOf(marker));
  });
}

bool confusesRoles(const std::string& reply)
{
  return std::ranges::any_of(kRoleLabels, [&reply](std::string_view label) { return reply.find(label) != std::string::npos; });
}

struct TurnVerdict
{
  bool claims{false};
  bool rawClaims{false};
  bool clockRestraint{false};
  bool recital{false};
  bool sentences{false};
  bool language{false};
  bool roleConfusion{false};
  bool genericOffer{false};
  bool parrot{false};
  bool relevance{false};
  bool unpromptedGreeting{false};
  bool missedNameAsk{false};
  bool nameAskRepeated{false};
  bool missedFact{false};
  bool leakedFact{false};
  bool actWrong{false};
};

struct DimensionField
{
  std::string_view name;
  bool TurnVerdict::*flag;
};

constexpr std::array<DimensionField, 16> kDimensions{{
    {.name = "claims", .flag = &TurnVerdict::claims},
    {.name = "rawClaims", .flag = &TurnVerdict::rawClaims},
    {.name = "clockRestraint", .flag = &TurnVerdict::clockRestraint},
    {.name = "recital", .flag = &TurnVerdict::recital},
    {.name = "sentences", .flag = &TurnVerdict::sentences},
    {.name = "language", .flag = &TurnVerdict::language},
    {.name = "roleConfusion", .flag = &TurnVerdict::roleConfusion},
    {.name = "genericOffer", .flag = &TurnVerdict::genericOffer},
    {.name = "parrot", .flag = &TurnVerdict::parrot},
    {.name = "relevance", .flag = &TurnVerdict::relevance},
    {.name = "unpromptedGreeting", .flag = &TurnVerdict::unpromptedGreeting},
    {.name = "missedNameAsk", .flag = &TurnVerdict::missedNameAsk},
    {.name = "nameAskRepeated", .flag = &TurnVerdict::nameAskRepeated},
    {.name = "missedFact", .flag = &TurnVerdict::missedFact},
    {.name = "leakedFact", .flag = &TurnVerdict::leakedFact},
    {.name = "actWrong", .flag = &TurnVerdict::actWrong},
}};

struct FactPresence
{
  const CallCase& item;
  std::string_view facet;
  const std::string& contextBlock;
};

bool facetPresent(const FactPresence& input)
{
  if (input.contextBlock.empty())
    return false;
  return std::ranges::any_of(input.item.facts, [&](const ContextFact& fact) {
    return fact.facet == input.facet && !fact.text.empty() &&
           input.contextBlock.find(fact.text) != std::string::npos;
  });
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

struct TurnScoreInput
{
  const std::string& reply;
  const std::string& rawReply;
  const std::string& contextBlock;
  const CallCase& item;
  const TurnState& state;
  const std::string& expectedAct;
  const std::string& emittedAct;
  bool firstTurn{false};
  bool checkFacts{false};
};

TurnVerdict scoreTurn(const TurnScoreInput& input)
{
  TurnVerdict verdict;
  verdict.claims = claimedWithoutTool(input.reply, input.state);
  verdict.rawClaims = claimedWithoutTool(input.rawReply, input.state);
  verdict.clockRestraint = !input.item.asksClock && (call_checks::namesADate(input.reply) || namesATime(input.reply));
  verdict.recital = call_checks::recitesNotes(input.reply, input.item.notes);
  verdict.sentences = call_checks::sentenceCount(input.reply) > kMaxSentences;
  verdict.language = speaksForeignTokens(input.reply, input.item.lang);
  verdict.roleConfusion = confusesRoles(input.reply);
  verdict.genericOffer = reply_claims::genericOffer({.text = input.reply, .lang = input.item.lang});
  verdict.parrot = call_checks::parrots(input.reply, input.item.noEcho);
  verdict.relevance = call_checks::missesExpectedToken(input.reply, input.item.expectAny);
  verdict.unpromptedGreeting = !input.item.greetsUser && call_checks::opensWithGreeting(input.reply);
  verdict.missedNameAsk = call_checks::missedNameAsk(
      {.reply = input.reply, .nameUnknown = input.item.nameUnknown, .firstTurn = input.firstTurn});
  verdict.nameAskRepeated = call_checks::nameAskRepeated({.reply = input.reply, .firstTurn = input.firstTurn});
  verdict.actWrong = !input.expectedAct.empty() && input.emittedAct != input.expectedAct;
  if (input.checkFacts) {
    const auto present = [&](std::string_view facet) {
      return facetPresent({.item = input.item, .facet = facet, .contextBlock = input.contextBlock});
    };
    verdict.missedFact = std::ranges::any_of(
        input.item.expectFacts, [&](const std::string& facet) { return !present(facet); });
    verdict.leakedFact = std::ranges::any_of(
        input.item.forbidFacts, [&](const std::string& facet) { return present(facet); });
  }
  return verdict;
}

struct TurnRecord
{
  std::string reply;
  std::string rawReply;
  std::string contextBlock;
  std::string act;
  std::string speech;
  std::string guardVerdict;
  std::string firstGuardVerdict;
  std::string firstRejected;
  std::string softVerdict;
  int attempts{0};
  std::vector<eval::RecordedCall> executed;
  TurnState state;
  TurnVerdict verdict;
  int64_t ms{0};
  int32_t promptTokens{0};
  int32_t decodedTokens{0};
  bool softRelease{false};
};

struct CaseRecord
{
  CallCase item;
  std::string staticPrefix;
  std::vector<TurnRecord> turns;
};

struct CaseInput
{
  LlmController& controller;
  const CallCase& item;
  const eval::StubInput& stubs;
  const std::vector<tools::ToolHandle>& offered;
  const std::string& prompt;
  const std::string& known;
  const std::string& roles;
  const std::vector<std::string>& dropped;
  float temperature{0.0F};
  bool legacyContext{false};
};

std::vector<ContextFact> offeredFacts(const CallCase& item, const std::vector<std::string>& dropped)
{
  std::vector<ContextFact> facts;
  for (const ContextFact& fact : item.facts)
    if (std::ranges::find(dropped, fact.facet) == dropped.end())
      facts.push_back(fact);
  return facts;
}

CaseRecord runCase(const CaseInput& input)
{
  CaseRecord run{.item = input.item, .staticPrefix = {}, .turns = {}};
  std::vector<ChatMessage> history;
  std::vector<std::string> legacyFacts;
  if (input.legacyContext)
    for (const ContextFact& fact : input.item.facts)
      legacyFacts.push_back(fact.text);
  run.staticPrefix = call_checks::staticPrompt({.roles = input.item.roles,
                                                .rolesDefault = input.roles,
                                                .prompt = input.prompt,
                                                .person = input.item.person,
                                                .known = input.known,
                                                .facts = legacyFacts});
  history.push_back({.role = "system", .content = run.staticPrefix});
  bool firstTurn = true;
  for (std::size_t index = 0; index < input.item.script.size(); ++index) {
    const std::string& utterance = input.item.script[index];
    history.push_back({.role = "user", .content = utterance});
    input.stubs.recorder->clear();
    ChatRequest request;
    request.messages = history;
    request.contextFacts = input.legacyContext ? std::vector<ContextFact>{} : offeredFacts(input.item, input.dropped);
    request.maxTokens = kMaxTokens;
    request.temperature = input.temperature;
    request.resetContext = firstTurn;
    request.userId = kEvalUser;
    request.role = UserRole::Owner;
    request.lang = input.item.lang;
    request.clientActions = true;
    request.sessionId = "call-faithfulness-" + input.item.id;
    const bool lastTurn = &utterance == &input.item.script.back();
    const auto started = std::chrono::steady_clock::now();
    const LlmChatOutcome outcome = input.controller.chatSync(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const LlmPrefillStats prefill = input.controller.service().lastPrefillStats();
    TurnRecord turn;
    turn.reply = outcome.text;
    turn.rawReply = outcome.rawReply;
    turn.contextBlock = outcome.contextBlock;
    turn.act = outcome.act;
    turn.speech = outcome.speech;
    turn.guardVerdict = outcome.guardVerdict;
    turn.firstGuardVerdict = outcome.firstGuardVerdict;
    turn.firstRejected = outcome.firstRejected;
    turn.softVerdict = outcome.softVerdict;
    turn.attempts = outcome.attempts;
    turn.softRelease = outcome.softRelease;
    turn.executed = input.stubs.recorder->executed();
    turn.ms = elapsed.count();
    turn.promptTokens = prefill.promptTokens;
    turn.decodedTokens = prefill.decodedTokens;
    turn.state = stateFor({.utterance = utterance, .offered = input.offered, .executed = turn.executed, .lang = input.item.lang});
    const std::string expectedAct = index < input.item.acts.size() ? input.item.acts[index] : std::string{};
    turn.verdict = scoreTurn({.reply = turn.reply,
                              .rawReply = turn.rawReply,
                              .contextBlock = turn.contextBlock,
                              .item = input.item,
                              .state = turn.state,
                              .expectedAct = expectedAct,
                              .emittedAct = turn.act,
                              .firstTurn = firstTurn,
                              .checkFacts = lastTurn && !input.legacyContext});
    history.push_back({.role = "assistant", .content = outcome.text});
    run.turns.push_back(std::move(turn));
    firstTurn = false;
  }
  return run;
}

std::vector<CallCase> selected(const std::vector<CallCase>& cases, const Options& options)
{
  std::vector<CallCase> out;
  for (const auto& item : cases) {
    if (!options.filter.empty() && item.id.find(options.filter) == std::string::npos && item.lang != options.filter)
      continue;
    out.push_back(item);
  }
  if (options.limit > 0 && out.size() > static_cast<size_t>(options.limit))
    out.resize(static_cast<size_t>(options.limit));
  return out;
}

struct SpeechTally
{
  int actTurns{0};
  int retried{0};
  int unavailable{0};
  int softReleased{0};
};

SpeechTally speechTally(const std::vector<CaseRecord>& runs)
{
  SpeechTally tally;
  for (const auto& run : runs) {
    for (const auto& turn : run.turns) {
      if (turn.act.empty())
        continue;
      ++tally.actTurns;
      if (!turn.firstGuardVerdict.empty() && turn.firstGuardVerdict != "pass")
        ++tally.retried;
      if (turn.speech == "unavailable")
        ++tally.unavailable;
      if (turn.softRelease)
        ++tally.softReleased;
    }
  }
  return tally;
}

eval::Metrics metricsOf(const std::vector<CaseRecord>& runs, const std::vector<std::string>& markers)
{
  std::map<std::string_view, int> violatingTurns;
  std::map<std::string_view, int> violatingCases;
  std::map<std::string, int> verdictCounts;
  for (const turn::speech::GuardVerdict verdict : {turn::speech::GuardVerdict::Pass,
                                                   turn::speech::GuardVerdict::NotAQuestion,
                                                   turn::speech::GuardVerdict::SlotNotAsked,
                                                   turn::speech::GuardVerdict::SlotReasked,
                                                   turn::speech::GuardVerdict::OptionsIncomplete,
                                                   turn::speech::GuardVerdict::ActionUnnamed,
                                                   turn::speech::GuardVerdict::ArgumentMissing,
                                                   turn::speech::GuardVerdict::NotYesOrNo,
                                                   turn::speech::GuardVerdict::ClaimedWithoutTool,
                                                   turn::speech::GuardVerdict::IdentifierLeaked})
    verdictCounts[std::string(turn::speech::verdictName(verdict))] = 0;
  int turns = 0;
  int passedCases = 0;
  for (const auto& run : runs) {
    std::vector<std::string_view> violated;
    for (const auto& turn : run.turns) {
      ++turns;
      if (!turn.guardVerdict.empty())
        ++verdictCounts[turn.guardVerdict];
      for (const DimensionField& dimension : kDimensions) {
        if (!(turn.verdict.*(dimension.flag)))
          continue;
        ++violatingTurns[dimension.name];
        if (std::ranges::find(violated, dimension.name) == violated.end())
          violated.push_back(dimension.name);
      }
    }
    for (std::string_view name : violated)
      ++violatingCases[name];
    passedCases += violated.empty() ? 1 : 0;
  }
  eval::Metrics metrics;
  metrics["cases"] = static_cast<double>(runs.size());
  metrics["turns"] = static_cast<double>(turns);
  metrics["casePass"] = static_cast<double>(passedCases);
  metrics["casePassRate"] =
      runs.empty() ? 0.0 : static_cast<double>(passedCases) / static_cast<double>(runs.size());
  for (const DimensionField& dimension : kDimensions) {
    metrics[std::string(dimension.name)] = static_cast<double>(violatingTurns[dimension.name]);
    metrics[std::string(dimension.name) + ".cases"] = static_cast<double>(violatingCases[dimension.name]);
  }
  const SpeechTally tally = speechTally(runs);
  metrics["speechTurns"] = static_cast<double>(tally.actTurns);
  metrics["retriedTurns"] = static_cast<double>(tally.retried);
  metrics["guardedTurns"] = static_cast<double>(tally.unavailable);
  metrics["softReleasedTurns"] = static_cast<double>(tally.softReleased);
  metrics["retryShare"] = tally.actTurns == 0 ? 0.0 : static_cast<double>(tally.retried) / static_cast<double>(tally.actTurns);
  metrics["unavailableShare"] =
      tally.actTurns == 0 ? 0.0 : static_cast<double>(tally.unavailable) / static_cast<double>(tally.actTurns);
  metrics["softReleaseShare"] =
      tally.actTurns == 0 ? 0.0 : static_cast<double>(tally.softReleased) / static_cast<double>(tally.actTurns);
  int regionalTurns = 0;
  int regionalCases = 0;
  for (const auto& run : runs) {
    bool hit = false;
    for (const auto& turn : run.turns)
      if (speaksRegionalism(turn.reply, markers)) {
        hit = true;
        ++regionalTurns;
      }
    if (hit)
      ++regionalCases;
  }
  metrics["regionalism"] = static_cast<double>(regionalTurns);
  metrics["regionalism.cases"] = static_cast<double>(regionalCases);
  for (const auto& [name, count] : verdictCounts)
    metrics["guard." + name] = static_cast<double>(count);
  return metrics;
}

struct RejectSampleInput
{
  const std::string& path;
  const std::vector<CaseRecord>& runs;
};

bool writeRejectSample(const RejectSampleInput& input)
{
  std::ofstream out(input.path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["emitUTF8"] = true;
  for (const CaseRecord& run : input.runs) {
    for (std::size_t index = 0; index < run.turns.size(); ++index) {
      const TurnRecord& turn = run.turns[index];
      if (turn.firstGuardVerdict.empty() || turn.firstGuardVerdict == "pass")
        continue;
      Json::Value row(Json::objectValue);
      row["id"] = run.item.id;
      row["lang"] = run.item.lang;
      row["turn"] = static_cast<int>(index);
      row["act"] = turn.act;
      row["verdict"] = turn.firstGuardVerdict;
      row["reply"] = turn.firstRejected;
      out << Json::writeString(builder, row) << "\n";
    }
  }
  return static_cast<bool>(out);
}

std::string violationsOf(const TurnVerdict& verdict)
{
  std::string out;
  for (const DimensionField& dimension : kDimensions) {
    if (!(verdict.*(dimension.flag)))
      continue;
    out += out.empty() ? std::string(dimension.name) : " " + std::string(dimension.name);
  }
  return out.empty() ? std::string("ok") : out;
}

std::string describe(const CaseRecord& run)
{
  std::string out = std::format("{} [{}] camera={} clock={}", run.item.id, run.item.lang, run.item.asksCamera, run.item.asksClock);
  for (const auto& turn : run.turns) {
    std::string calls;
    for (const auto& call : turn.executed)
      calls += call.tool + " ";
    out += std::format("\n  {}ms | {} | called: {} | {}", turn.ms, violationsOf(turn.verdict), calls.empty() ? "-" : calls, turn.reply);
    if (turn.verdict.rawClaims || turn.verdict.genericOffer)
      out += std::format("\n    raw: {}", turn.rawReply);
  }
  return out;
}

struct DumpInput
{
  const std::string& path;
  LlmService& service;
  const std::vector<CaseRecord>& runs;
};

bool writeDump(const DumpInput& input)
{
  std::ofstream out(input.path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  for (const CaseRecord& run : input.runs) {
    const int32_t prefixTokens = input.service.countTokens(run.staticPrefix);
    for (std::size_t index = 0; index < run.turns.size(); ++index) {
      const TurnRecord& turn = run.turns[index];
      Json::Value row(Json::objectValue);
      row["id"] = run.item.id;
      row["lang"] = run.item.lang;
      row["turn"] = static_cast<int>(index);
      row["prefixTokens"] = prefixTokens;
      row["contextTokens"] = input.service.countTokens(turn.contextBlock);
      row["promptTokens"] = turn.promptTokens;
      row["decodedTokens"] = turn.decodedTokens;
      row["ms"] = Json::Value::Int64(turn.ms);
      row["facts"] = Json::Value(Json::arrayValue);
      for (const ContextFact& fact : run.item.facts)
        row["facts"].append(fact.facet);
      row["contextBlock"] = turn.contextBlock;
      out << Json::writeString(builder, row) << "\n";
    }
  }
  return static_cast<bool>(out);
}

struct TtftInput
{
  LlmController& controller;
  const std::vector<CallCase>& cases;
  const std::vector<std::string>& dropped;
  float temperature{0.0F};
  std::string path;
};

bool runTtft(const TtftInput& input)
{
  std::ofstream out(input.path);
  if (!out)
    return false;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  for (const CallCase& item : input.cases) {
    std::vector<ChatMessage> history;
    history.push_back({.role = "system", .content = "persona"});
    int turn = 0;
    for (const auto& utterance : item.script) {
      history.push_back({.role = "user", .content = utterance});
      ChatRequest request;
      request.messages = history;
      request.contextFacts = offeredFacts(item, input.dropped);
      request.maxTokens = kMaxTokens;
      request.temperature = input.temperature;
      request.resetContext = turn == 0;
      request.userId = kEvalUser;
      request.role = UserRole::Owner;
      request.lang = item.lang;
      request.clientActions = true;
      request.sessionId = "call-faithfulness-" + item.id;
      LlmPrefillStats stats;
      std::chrono::steady_clock::time_point first{};
      std::string reply;
      const auto started = std::chrono::steady_clock::now();
      input.controller.chatStreamSync(
          {.request = request,
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
      row["lang"] = item.lang;
      row["turn"] = turn;
      row["firstTokenMs"] = Json::Value::Int64(firstMs);
      row["promptTokens"] = stats.promptTokens;
      row["decodedTokens"] = stats.decodedTokens;
      out << Json::writeString(builder, row) << "\n";
      history.push_back({.role = "assistant", .content = reply});
      ++turn;
    }
  }
  return static_cast<bool>(out);
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

Engines setupEngines(LlmController& controller, const Options& options)
{
  Engines engines;
  if (!options.decide.empty())
    ConfigService::setRuntimeString("decide.engine", options.decide);
  if (!options.decideBundle.empty())
    ConfigService::setRuntimeString("decide.laya.bundle_dir", options.decideBundle);
  if (!options.layaAgentConfig.empty())
    ConfigService::setRuntimeString("decide.laya.agent_config", options.layaAgentConfig);
  if (!options.extract.empty())
    ConfigService::setRuntimeString("extract.engine", options.extract);
  if (!options.extractBundle.empty())
    ConfigService::setRuntimeString("extract.gliner.bundle_dir", options.extractBundle);
  if (options.decide == "laya") {
    const turn::BundleLoader bundle({.dir = options.decideBundle, .pin = bundlePin(options.decideBundle)});
    if (bundle.valid() && !turn::unknownLayaLabel(bundle.labels())) {
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
      engines.gliner = std::make_unique<turn::GlinerExtractor>(turn::GlinerExtractorInput{
          .model = turn::openGlinerModel(bundle, {}), .fallback = nullptr, .thresholds = bundle.thresholds()});
      controller.adapter().flow().useText(*engines.gliner);
      engines.active += (engines.active.empty() ? "" : "+");
      engines.active += "gliner";
    }
    else {
      std::cout << std::format("[WARN] the GLiNER bundle is off: {}\n", bundle.error());
    }
  }
  if (engines.active.empty())
    engines.active = "rules+router+nuextract";
  return engines;
}

struct RenderCase
{
  std::string id;
  std::string lang;
  std::string variant;
  std::string utterance;
  turn::speech::Act act;
  bool wrote{false};
};

turn::speech::AskSlot askSlotOf(std::string slot, std::string tool, turn::speech::AskReason reason)
{
  turn::speech::AskSlot ask;
  ask.slot = std::move(slot);
  ask.tool = std::move(tool);
  ask.reason = reason;
  return ask;
}

std::string isoOf(int64_t epoch)
{
  return iso_time::format(epoch);
}

std::vector<RenderCase> renderCases(int64_t now)
{
  using turn::speech::Act;
  using turn::speech::AskReason;
  using turn::speech::AskSlot;
  using turn::speech::Choose;
  using turn::speech::Confirm;
  using turn::speech::DateKind;
  using turn::speech::DatePart;
  using turn::speech::Declined;
  using turn::speech::Done;
  using turn::speech::Misunderstood;
  using turn::speech::Offer;
  using turn::speech::Refused;
  using turn::speech::Unactionable;

  const int64_t nextWeekEpoch = now + 7 * 86400;
  std::vector<RenderCase> out;
  const auto add = [&out](std::string id, std::string lang, std::string variant, std::string utterance, Act act) {
    out.push_back({.id = std::move(id),
                   .lang = std::move(lang),
                   .variant = std::move(variant),
                   .utterance = std::move(utterance),
                   .act = std::move(act)});
  };

  const auto addDone = [&out](std::string id, std::string lang, std::string variant, Done done) {
    const std::string utterance = lang == "en" ? "schedule a meeting with Andrea tomorrow at five"
                                                : "agéndame una reunión con Andrea mañana a las cinco";
    out.push_back({.id = std::move(id),
                   .lang = std::move(lang),
                   .variant = std::move(variant),
                   .utterance = utterance,
                   .act = std::move(done),
                   .wrote = true});
  };

  {
    AskSlot ask = askSlotOf("title", "task.create", AskReason::Missing);
    add("es-ask-title", "es", "neutral", "anota una tarea", ask);
    add("en-ask-title", "en", "en", "note a task", ask);
  }
  {
    AskSlot ask = askSlotOf("starts_at", "calendar.create_event", AskReason::Missing);
    add("es-ask-starts-at", "es", "neutral", "agéndame una reunión con Andrea", ask);
    add("en-ask-starts-at", "en", "en", "schedule a meeting with Andrea", ask);
  }
  {
    AskSlot ask = askSlotOf("starts_at", "calendar.create_event", AskReason::AmbiguousDate);
    ask.dates = {DatePart{.kind = DateKind::Weekday, .epoch = nextWeekEpoch},
                 DatePart{.kind = DateKind::CalendarDate, .epoch = now}};
    add("es-ask-ambiguous", "es", "neutral", "agéndame la reunión el jueves", ask);
    add("en-ask-ambiguous", "en", "en", "schedule the meeting on Thursday", ask);
  }
  {
    AskSlot ask = askSlotOf("project", "task.create", AskReason::ProjectChoice);
    ask.options = {"casa", "trabajo", "gimnasio"};
    add("es-ask-project", "es", "neutral", "anota una tarea en el proyecto casa", ask);
    AskSlot askEn = askSlotOf("project", "task.create", AskReason::ProjectChoice);
    askEn.options = {"home", "work", "gym"};
    add("en-ask-project", "en", "en", "note a task in the project home", askEn);
  }
  {
    Confirm confirm;
    confirm.action = "calendar.cancel_event";
    confirm.args["title"] = "Cita con el dentista";
    confirm.irreversible = true;
    confirm.toolPreview = "Esto cancelaría «Cita con el dentista».";
    add("es-command-cancel", "es", "neutral", "cancela la cita con el dentista", confirm);
    Confirm confirmEn;
    confirmEn.action = "calendar.cancel_event";
    confirmEn.args["title"] = "Dentist appointment";
    confirmEn.irreversible = true;
    confirmEn.toolPreview = "This would cancel \"Dentist appointment\".";
    add("en-command-cancel", "en", "en", "cancel the dentist appointment", confirmEn);
  }
  {
    Choose choose;
    choose.options = {"calendar.create_event", "task.create"};
    add("es-command-choose", "es", "neutral", "apunta la reunión con Andrea", choose);
    add("en-command-choose", "en", "en", "note the meeting with Andrea", choose);
  }
  {
    Done es;
    es.tool = "calendar.create_event";
    es.fact = "Agendé «Reunión con Andrea» para " + isoOf(now + 86400) + ".";
    es.readback = "Reunión con Andrea";
    addDone("es-command-done", "es", "neutral", es);
    Done en;
    en.tool = "calendar.create_event";
    en.fact = "Scheduled \"Meeting with Andrea\" for " + isoOf(now + 86400) + ".";
    en.readback = "Meeting with Andrea";
    addDone("en-command-done", "en", "en", en);
  }
  {
    Refused refused{.tool = "calendar.cancel_event", .reason = "not_found"};
    add("es-command-refused", "es", "neutral", "cancela la reunión del dentista", refused);
    add("en-command-refused", "en", "en", "cancel the dentist meeting", refused);
  }
  {
    Offer offer;
    offer.module = "productivity";
    offer.name = "Productividad";
    offer.facts = "Productividad lleva tus proyectos y tus tareas.";
    offer.pendingIntent = "anotar una tarea";
    add("es-command-offer", "es", "neutral", "anota una tarea en productividad", offer);
    Offer offerEn;
    offerEn.module = "productivity";
    offerEn.name = "Productivity";
    offerEn.facts = "Productivity keeps your projects and your tasks.";
    offerEn.pendingIntent = "note a task";
    add("en-command-offer", "en", "en", "note a task in productivity", offerEn);
  }
  add("es-command-declined", "es", "neutral", "no, déjalo", Declined{});
  add("en-command-declined", "en", "en", "no, leave it", Declined{});
  add("es-command-unactionable", "es", "neutral", "cuéntame un chiste", Unactionable{.reason = "no_matching_action"});
  add("en-command-unactionable", "en", "en", "tell me a joke", Unactionable{.reason = "no_matching_action"});
  add("es-command-misunderstood", "es", "neutral", "eh", Misunderstood{});
  add("en-command-misunderstood", "en", "en", "uh", Misunderstood{});
  {
    AskSlot twin = askSlotOf("event_id", "calendar.cancel_event", AskReason::Missing);
    twin.knownArgs["task_id"] = 12;
    add("es-twin-event-task", "es", "neutral", "cancela la reunión con el dentista, no la tarea", twin);
    add("en-twin-event-task", "en", "en", "cancel the meeting with the dentist, not the task", twin);
  }
  {
    AskSlot twin = askSlotOf("environment", "app.set_guard_mode", AskReason::Missing);
    twin.knownArgs["location"] = "casa";
    add("es-twin-place", "es", "neutral", "pon la vigilancia en modo noche en la casa", twin);
    AskSlot twinEn = askSlotOf("environment", "app.set_guard_mode", AskReason::Missing);
    twinEn.knownArgs["location"] = "home";
    add("en-twin-place", "en", "en", "set the guard to night mode at home", twinEn);
  }
  {
    AskSlot ask = askSlotOf("when", "memory.remind", AskReason::Missing);
    add("pe-ask-when", "es", "pe", "recuérdame llamar a mi mamá pe", ask);
    Confirm confirm;
    confirm.action = "calendar.cancel_event";
    confirm.args["title"] = "Cita con el dentista";
    confirm.irreversible = true;
    confirm.toolPreview = "Esto cancelaría «Cita con el dentista».";
    add("pe-command-cancel", "es", "pe", "cancela la cita con el dentista pe", confirm);
    Choose choose;
    choose.options = {"calendar.create_event", "task.create"};
    add("pe-command-choose", "es", "pe", "apunta la reunión con Andrea pe", choose);
  }

  return out;
}

struct RenderRecord
{
  RenderCase item;
  std::string reply;
  std::string speech;
  std::string finalVerdict;
  std::string firstVerdict;
  std::string firstRejected;
  int attempts{0};
  bool softRelease{false};
};

struct RenderRunInput
{
  LfmAdapter& adapter;
  const std::vector<RenderCase>& cases;
  const std::string& prompt;
  int64_t now{0};
  float temperature{0.0F};
};

RenderRecord renderOne(const RenderRunInput& input, const RenderCase& item)
{
  using turn::speech::Speech;
  Speech speech{.acts = {item.act}, .lang = item.lang, .now = input.now};
  const std::string tail = turn::speech::actTail({.speech = speech, .contextBlock = {}});
  ToolChatInput loop;
  loop.audience = {.role = UserRole::Owner, .modules = {}};
  loop.context = {.userId = kEvalUser,
                  .role = UserRole::Owner,
                  .lang = item.lang,
                  .sessionId = "speech-acts-" + item.id,
                  .channel = "tool_result",
                  .utterance = item.utterance,
                  .decided = false,
                  .turn = 1,
                  .emitAction = {}};
  loop.temperature = input.temperature;
  loop.resetContext = false;
  loop.answerMaxTokens = kMaxTokens;
  std::vector<ChatMessage> history;
  history.push_back({.role = "system", .content = input.prompt});
  history.push_back({.role = "user", .content = item.utterance});
  ToolChatOutput output;
  input.adapter.speakAct({.input = loop,
                          .history = history,
                          .onToken = nullptr,
                          .speech = speech,
                          .tail = tail,
                          .asked = false,
                          .wrote = item.wrote,
                          .opened = false,
                          .callsConfirmed = false},
                         output);
  return {.item = item,
          .reply = output.reply,
          .speech = output.speech,
          .finalVerdict = output.guardVerdict,
          .firstVerdict = output.firstGuardVerdict,
          .firstRejected = output.firstRejected,
          .attempts = output.attempts,
          .softRelease = output.softRelease};
}

eval::Metrics renderMetrics(const std::vector<RenderRecord>& runs, const std::vector<std::string>& markers)
{
  eval::Metrics metrics;
  int passed = 0;
  int rejectedFirst = 0;
  int unavailable = 0;
  int softReleased = 0;
  std::map<std::string, int> verdictCounts;
  std::map<std::string, int> actCounts;
  for (const turn::speech::GuardVerdict verdict : {turn::speech::GuardVerdict::Pass,
                                                   turn::speech::GuardVerdict::NotAQuestion,
                                                   turn::speech::GuardVerdict::SlotNotAsked,
                                                   turn::speech::GuardVerdict::SlotReasked,
                                                   turn::speech::GuardVerdict::OptionsIncomplete,
                                                   turn::speech::GuardVerdict::ActionUnnamed,
                                                   turn::speech::GuardVerdict::ArgumentMissing,
                                                   turn::speech::GuardVerdict::NotYesOrNo,
                                                   turn::speech::GuardVerdict::ClaimedWithoutTool,
                                                   turn::speech::GuardVerdict::IdentifierLeaked})
    verdictCounts[std::string(turn::speech::verdictName(verdict))] = 0;
  for (const auto& run : runs) {
    const bool ok = run.finalVerdict == "pass" || run.softRelease;
    passed += ok ? 1 : 0;
    if (!run.firstVerdict.empty() && run.firstVerdict != "pass")
      ++rejectedFirst;
    if (run.speech == "unavailable")
      ++unavailable;
    if (run.softRelease)
      ++softReleased;
    ++verdictCounts[run.finalVerdict];
    const std::string act(turn::speech::actName(run.item.act));
    ++actCounts[act];
  }
  metrics["cases"] = static_cast<double>(runs.size());
  metrics["pass"] = static_cast<double>(passed);
  metrics["rejectedFirst"] = static_cast<double>(rejectedFirst);
  metrics["retryShare"] = runs.empty() ? 0.0 : static_cast<double>(rejectedFirst) / static_cast<double>(runs.size());
  metrics["unavailable"] = static_cast<double>(unavailable);
  metrics["unavailableShare"] = runs.empty() ? 0.0 : static_cast<double>(unavailable) / static_cast<double>(runs.size());
  metrics["softRelease"] = static_cast<double>(softReleased);
  metrics["softReleaseShare"] = runs.empty() ? 0.0 : static_cast<double>(softReleased) / static_cast<double>(runs.size());
  int regional = 0;
  for (const auto& run : runs)
    if (speaksRegionalism(run.reply, markers))
      ++regional;
  metrics["regionalism"] = static_cast<double>(regional);
  metrics["regionalism.cases"] = static_cast<double>(regional);
  for (const auto& [name, count] : verdictCounts)
    metrics["verdict." + name] = static_cast<double>(count);
  for (const auto& [name, count] : actCounts)
    metrics["act." + name] = static_cast<double>(count);
  return metrics;
}

struct RenderRunCli
{
  const Options& options;
  const GatesState& gates;
  LfmAdapter& adapter;
  const std::string& promptEs;
  const std::string& promptEn;
  const std::vector<std::string>& markers;
  int64_t now{0};
};

int runRendering(const RenderRunCli& input)
{
  const std::vector<RenderCase> cases = renderCases(input.now);
  std::vector<RenderRecord> runs;
  runs.reserve(cases.size());
  for (const RenderCase& item : cases) {
    runs.push_back(renderOne({.adapter = input.adapter,
                              .cases = cases,
                              .prompt = item.lang == "en" ? input.promptEn : input.promptEs,
                              .now = input.now,
                              .temperature = input.options.temperature},
                             item));
    const RenderRecord& run = runs.back();
    if (input.options.verbose)
      std::cout << std::format("{} [{}] {} attempts={} first={} final={} | {}\n",
                               run.item.id,
                               run.item.lang,
                               run.item.variant,
                               run.attempts,
                               run.firstVerdict,
                               run.finalVerdict,
                               run.reply);
  }
  eval::Metrics metrics = renderMetrics(runs, input.markers);
  metrics["now"] = static_cast<double>(input.now);
  std::cout << std::format("speech acts: {} rendering cases\n", runs.size());
  eval::printMetrics(metrics);
  if (!input.options.rejectSample.empty()) {
    std::ofstream out(input.options.rejectSample);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    for (const RenderRecord& run : runs) {
      if (run.firstVerdict.empty() || run.firstVerdict == "pass")
        continue;
      Json::Value row(Json::objectValue);
      row["id"] = run.item.id;
      row["lang"] = run.item.lang;
      row["variant"] = run.item.variant;
      row["act"] = std::string(turn::speech::actName(run.item.act));
      row["verdict"] = run.firstVerdict;
      row["reply"] = run.firstRejected;
      out << Json::writeString(builder, row) << "\n";
    }
  }
  const eval::LoadedGates gates = eval::loadGates(input.options.gates, "speechActs");
  if (!gates.error.empty()) {
    std::cout << "[ERROR] " << gates.error << "\n";
    return 1;
  }
  const eval::Verdict verdict = eval::check(gates.gates, metrics);
  if (!input.options.report.empty() &&
      !eval::writeReport(input.options.report,
                         {.section = "speechActs", .metrics = metrics, .verdict = verdict, .pinned = input.gates.pinned})) {
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

struct FinishInput
{
  const Options& options;
  const GatesState& gates;
  const std::vector<CaseRecord>& runs;
  const std::vector<std::string>& markers;
};

int finish(const FinishInput& input)
{
  if (!input.gates.pinned) {
    std::cout << "[SKIPPED] no callFaithfulness gates pinned\n";
    return kSkipped;
  }
  const eval::Metrics metrics = metricsOf(input.runs, input.markers);
  eval::Verdict verdict;
  {
    const eval::LoadedGates gates = eval::loadGates(input.options.gates, "callFaithfulness");
    if (!gates.error.empty()) {
      std::cout << "[ERROR] " << gates.error << "\n";
      return 1;
    }
    verdict = eval::check(gates.gates, metrics);
  }
  std::cout << std::format("call faithfulness: {} cases, {} turns\n", input.runs.size(), metrics.at("turns"));
  eval::printMetrics(metrics);
  if (!input.options.report.empty() &&
      !eval::writeReport(input.options.report,
                         {.section = "callFaithfulness", .metrics = metrics, .verdict = verdict, .pinned = input.gates.pinned})) {
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
                             options.gates, gates.temperature, gates.seed, options.temperature, options.seed);
    return kSkipped;
  }
  const std::vector<std::string> markers = options.regionalismMarkers.empty()
                                               ? std::vector<std::string>{}
                                               : loadRegionalismMarkers(options.regionalismMarkers);
  if (!options.regionalismMarkers.empty() && markers.empty()) {
    std::cout << "[ERROR] no regionalism markers in " << options.regionalismMarkers << "\n";
    return 1;
  }
  if (!std::filesystem::exists(options.llmModel)) {
    std::cout << "[SKIPPED] no LLM weights at " << options.llmModel << "; the call-faithfulness gate did not run\n";
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
  const LoadedCallCases loaded = loadCallCases(options.cases);
  if (!loaded.error.empty()) {
    std::cout << "[ERROR] " << loaded.error << "\n";
    return 1;
  }
  const std::string catalogPath = catalogPathFor(options.cases);
  const ModuleFlags catalog = catalogFlags(catalogPath);
  if (catalog.empty()) {
    std::cout << "[ERROR] no module catalog at " << catalogPath << "\n";
    return 1;
  }

  const std::string scratch =
      writeScratchConfig({.model = options.llmModel, .temperature = options.temperature, .seed = options.seed});
  ConfigService::load(scratch);
  std::filesystem::remove(scratch);
  const std::filesystem::path intentModel =
      std::filesystem::path(options.llmModel).parent_path().parent_path() / "intent" / "intent.bin";
  if (std::filesystem::exists(intentModel))
    ConfigService::setRuntimeString("intent.model_file", intentModel.string());
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

  moduleGate().apply(catalog);
  const std::vector<tools::ToolHandle> offered =
      controller.adapter().executor().offered({.role = UserRole::Owner, .modules = moduleGate().snapshot()});
  const Engines engines = setupEngines(controller, options);
  std::cout << std::format("pipeline: {}\n", engines.active);
  if ((options.decide == "laya" && engines.laya == nullptr) || (options.extract == "gliner" && engines.gliner == nullptr)) {
    std::cout << "[REFUSED] an engine the run asked for is not up; no gate is reported\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }

  if (options.renderActs) {
    const int result = runRendering({.options = options,
                                     .gates = gates,
                                     .adapter = controller.adapter(),
                                     .promptEs = spanish.bytes,
                                     .promptEn = english.bytes,
                                     .markers = markers,
                                     .now = call_checks::renderInstant(options.now)});
    controller.shutdownEngine();
    llama_backend_free();
    return result;
  }

  const std::vector<CallCase> cases = selected(loaded.cases, options);
  std::cout << std::format("call faithfulness: {} cases of {}\n", cases.size(), loaded.cases.size());
  std::vector<CaseRecord> runs;
  runs.reserve(cases.size());
  for (const auto& item : cases) {
    const bool englishCase = item.lang == "en";
    runs.push_back(runCase({.controller = controller,
                            .item = item,
                            .stubs = stubs,
                            .offered = offered,
                            .prompt = englishCase ? english.bytes : spanish.bytes,
                            .known = englishCase ? knownEn.bytes : knownEs.bytes,
                            .roles = englishCase ? rolesEn : rolesEs,
                            .dropped = options.dropFacets,
                            .temperature = options.temperature,
                            .legacyContext = options.legacyContext}));
    if (options.verbose)
      std::cout << describe(runs.back()) << "\n";
  }

  const int64_t layaRss = engines.laya != nullptr ? engines.laya->warmRssBytes() : 0;
  const int64_t glinerRss = engines.gliner != nullptr ? engines.gliner->warmRssBytes() : 0;
  std::cout << std::format("consumption: laya_rss_mb={} gliner_rss_mb={} models_rss_mb={} process_rss_mb={} pipeline={}\n",
                           static_cast<double>(layaRss) / (1024.0 * 1024.0),
                           static_cast<double>(glinerRss) / (1024.0 * 1024.0),
                           static_cast<double>(layaRss + glinerRss) / (1024.0 * 1024.0),
                           static_cast<double>(turn::residentBytes()) / (1024.0 * 1024.0),
                           engines.active);
  if (!options.dump.empty() && !writeDump({.path = options.dump, .service = controller.service(), .runs = runs})) {    std::cout << "[ERROR] cannot write " << options.dump << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }

  if (!options.ttft.empty() &&
      !runTtft({.controller = controller,
                .cases = cases,
                .dropped = options.dropFacets,
                .temperature = options.temperature,
                .path = options.ttft})) {
    std::cout << "[ERROR] cannot write " << options.ttft << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }

  if (!options.rejectSample.empty() && !writeRejectSample({.path = options.rejectSample, .runs = runs})) {
    std::cout << "[ERROR] cannot write " << options.rejectSample << "\n";
    controller.shutdownEngine();
    llama_backend_free();
    return 1;
  }

  controller.shutdownEngine();
  llama_backend_free();
  return finish({.options = options, .gates = gates, .runs = runs, .markers = markers});
}
