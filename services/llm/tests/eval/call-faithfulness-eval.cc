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
#include <feature/llm/services/turn/tool-effects.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <llm/llm-service.hxx>
#include <mcp/confirmation.hxx>

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
  std::string filter;
  float temperature{0.0F};
  uint32_t seed{42};
  int limit{0};
  bool verbose{false};
  bool force{false};
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
    else if (arg == "--filter" && hasValue)
      options.filter = argv[++i];
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

struct CallCase
{
  std::string id;
  std::string lang;
  std::vector<std::string> notes;
  std::vector<std::string> script;
  std::vector<std::string> noEcho;
  std::vector<std::string> expectAny;
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
    item.notes = stringList(node["notes"]);
    item.script = stringList(node["script"]);
    item.noEcho = stringList(node["noEcho"]);
    item.expectAny = stringList(node["expectAny"]);
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
      << "kv_type = \"auto\"\nflash_attn = \"auto\"\nthreads = 0\nbatch_threads = 0\n"
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
};

struct DimensionField
{
  std::string_view name;
  bool TurnVerdict::*flag;
};

constexpr std::array<DimensionField, 13> kDimensions{{
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
}};

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
  const CallCase& item;
  const TurnState& state;
  bool firstTurn{false};
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
  return verdict;
}

struct TurnRecord
{
  std::string reply;
  std::string rawReply;
  std::vector<eval::RecordedCall> executed;
  TurnState state;
  TurnVerdict verdict;
  int64_t ms{0};
};

struct CaseRecord
{
  CallCase item;
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
  float temperature{0.0F};
};

CaseRecord runCase(const CaseInput& input)
{
  CaseRecord run{.item = input.item, .turns = {}};
  std::vector<ChatMessage> history;
  history.push_back({.role = "system",
                     .content = call_checks::staticPrompt({.roles = input.item.roles,
                                                           .rolesDefault = input.roles,
                                                           .prompt = input.prompt,
                                                           .person = input.item.person,
                                                           .known = input.known,
                                                           .notes = input.item.notes})});
  bool firstTurn = true;
  for (const auto& utterance : input.item.script) {
    history.push_back({.role = "user", .content = utterance});
    input.stubs.recorder->clear();
    ChatRequest request;
    request.messages = history;
    request.maxTokens = kMaxTokens;
    request.temperature = input.temperature;
    request.resetContext = firstTurn;
    request.userId = kEvalUser;
    request.role = UserRole::Owner;
    request.lang = input.item.lang;
    request.clientActions = true;
    request.sessionId = "call-faithfulness-" + input.item.id;
    const auto started = std::chrono::steady_clock::now();
    const LlmChatOutcome outcome = input.controller.chatSync(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    TurnRecord turn;
    turn.reply = outcome.text;
    turn.rawReply = outcome.rawReply;
    turn.executed = input.stubs.recorder->executed();
    turn.ms = elapsed.count();
    turn.state = stateFor({.utterance = utterance, .offered = input.offered, .executed = turn.executed, .lang = input.item.lang});
    turn.verdict = scoreTurn({.reply = turn.reply,
                              .rawReply = turn.rawReply,
                              .item = input.item,
                              .state = turn.state,
                              .firstTurn = firstTurn});
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

eval::Metrics metricsOf(const std::vector<CaseRecord>& runs)
{
  std::map<std::string_view, int> violatingTurns;
  std::map<std::string_view, int> violatingCases;
  int turns = 0;
  int passedCases = 0;
  for (const auto& run : runs) {
    std::vector<std::string_view> violated;
    for (const auto& turn : run.turns) {
      ++turns;
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
  return metrics;
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

struct FinishInput
{
  const Options& options;
  const GatesState& gates;
  const std::vector<CaseRecord>& runs;
};

int finish(const FinishInput& input)
{
  if (!input.gates.pinned) {
    std::cout << "[SKIPPED] no callFaithfulness gates pinned\n";
    return kSkipped;
  }
  const eval::Metrics metrics = metricsOf(input.runs);
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
                            .temperature = options.temperature}));
    if (options.verbose)
      std::cout << describe(runs.back()) << "\n";
  }

  controller.shutdownEngine();
  llama_backend_free();
  return finish({.options = options, .gates = gates, .runs = runs});
}
