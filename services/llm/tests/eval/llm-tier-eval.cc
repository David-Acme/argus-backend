#include "eval-case.hxx"
#include "eval-report.hxx"
#include "eval-score.hxx"
#include "eval-tools.hxx"

#include <auth/module-gate.hxx>
#include <config/config-service.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/tools/tool-access.hxx>

#include <json/reader.h>
#include <json/writer.h>

#include <llama.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr int kSkipped = 77;
constexpr int64_t kEvalUser = 7;
constexpr int kMaxTokens = 160;

constexpr std::string_view kPersonaEs =
    "Eres Argus, un asistente de voz cálido y natural para un sistema de cámaras de seguridad local.\n"
    "Responde siempre en español, en dos frases cortas como máximo. Tu respuesta se dice en voz alta.\n"
    "Cámaras: Garaje, Puerta, Patio.";
constexpr std::string_view kPersonaEn =
    "You are Argus, a warm, natural home voice assistant for a local security camera system.\n"
    "Reply strictly in English, in at most two short sentences. Your reply is spoken aloud.\n"
    "Cameras: Garage, Door, Yard.";

struct Options
{
  std::string cases;
  std::string gates;
  std::string llmModel;
  std::string intentModel;
  std::string catalog;
  std::string report;
  std::string dump;
  std::string filter;
  std::string offer;
  std::string runsOut;
  std::string score;
  int skip = 0;
  int limit = 0;
  bool smoke = false;
  bool verbose = false;
  bool force = false;
  bool noFastTier = false;
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
    else if (arg == "--intent-model" && hasValue)
      options.intentModel = argv[++i];
    else if (arg == "--catalog" && hasValue)
      options.catalog = argv[++i];
    else if (arg == "--report" && hasValue)
      options.report = argv[++i];
    else if (arg == "--dump" && hasValue)
      options.dump = argv[++i];
    else if (arg == "--filter" && hasValue)
      options.filter = argv[++i];
    else if (arg == "--runs-out" && hasValue)
      options.runsOut = argv[++i];
    else if (arg == "--score" && hasValue)
      options.score = argv[++i];
    else if (arg == "--skip" && hasValue)
      options.skip = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (arg == "--offer" && hasValue)
      options.offer = argv[++i];
    else if (arg == "--limit" && hasValue)
      options.limit = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
    else if (arg == "--smoke")
      options.smoke = true;
    else if (arg == "--verbose")
      options.verbose = true;
    else if (arg == "--force")
      options.force = true;
    else if (arg == "--no-fast-tier")
      options.noFastTier = true;
  }
  return options;
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

ModuleFlags flagsFor(const ModuleFlags& catalog, const eval::EvalCase& item)
{
  ModuleFlags out = catalog;
  for (auto& flag : out) {
    if (flag.id == "core")
      continue;
    flag.enabled = std::ranges::find(item.modules, flag.id) != item.modules.end();
    flag.lifecycle = flag.enabled ? "active" : "disabled";
  }
  return out;
}

std::string writeScratchConfig(const std::string& model)
{
  const std::string path =
      (std::filesystem::temp_directory_path() / ("llm-tier-eval-" + std::to_string(::getpid()) + ".toml")).string();
  std::ofstream out(path);
  out << "[llm]\nmodel_path = \"" << model << "\"\n"
      << "context_size = 8192\nmax_tokens = " << kMaxTokens << "\ntemperature = 0.0\n"
      << "top_k = 20\ntop_p = 0.8\nmin_p = 0.0\npenalty_last_n = 64\npenalty_repeat = 1.10\n"
      << "seed = 42\nchat_template = \"chatml\"\ngpu_layers = -1\nn_batch = 1024\nn_ubatch = 512\n"
      << "kv_type = \"auto\"\nflash_attn = \"auto\"\nthreads = 0\nbatch_threads = 0\n"
      << "[drogon.app]\nnumber_of_threads = 2\n";
  return path;
}

ChatRequest requestFor(const std::vector<ChatMessage>& history, const eval::EvalCase& item)
{
  ChatRequest request;
  request.messages.push_back({.role = "system", .content = std::string(item.lang == "en" ? kPersonaEn : kPersonaEs)});
  request.messages.insert(request.messages.end(), history.begin(), history.end());
  request.maxTokens = kMaxTokens;
  request.temperature = 0.0F;
  request.userId = kEvalUser;
  request.role = userRoleFromString(item.role);
  request.lang = item.lang;
  request.clientActions = true;
  request.sessionId = "eval-" + item.id;
  return request;
}

std::vector<eval::RecordedCall> inactiveAttempts(const LlmChatOutcome& outcome, const eval::EvalCase& item)
{
  const ToolAudience audience{.role = userRoleFromString(item.role), .modules = moduleGate().snapshot()};
  std::vector<eval::RecordedCall> out;
  for (const auto& call : outcome.attempted) {
    const auto tool = ToolRegistry::instance().find(call.name);
    if (tool && tool_access::moduleInactive(audience, tool->spec))
      out.push_back({.tool = call.name, .arguments = call.arguments});
  }
  return out;
}

eval::CaseRun runCase(LlmController& controller, const eval::EvalCase& item, const eval::StubInput& stubs)
{
  eval::CaseRun run{.item = item, .turns = {}};
  std::vector<ChatMessage> history;
  for (const auto& utterance : item.script) {
    history.push_back({.role = "user", .content = utterance});
    stubs.recorder->clear();
    const auto started = std::chrono::steady_clock::now();
    const LlmChatOutcome outcome = controller.chatSync(requestFor(history, item));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    eval::TurnResult turn;
    turn.reply = outcome.text;
    turn.executed = stubs.recorder->executed();
    turn.offered = inactiveAttempts(outcome, item);
    turn.previews = stubs.recorder->previews();
    turn.confirmed = stubs.recorder->confirmations();
    turn.ms = elapsed.count();
    history.push_back({.role = "assistant", .content = outcome.text});
    run.turns.push_back(std::move(turn));
  }
  return run;
}

std::vector<eval::EvalCase> selected(const std::vector<eval::EvalCase>& cases, const Options& options)
{
  std::vector<eval::EvalCase> out;
  for (const auto& item : cases) {
    if (!options.filter.empty() && item.id.find(options.filter) == std::string::npos && item.group != options.filter)
      continue;
    out.push_back(item);
  }
  if (options.smoke) {
    std::map<std::string, int> perGroup;
    std::erase_if(out, [&perGroup](const eval::EvalCase& item) { return ++perGroup[item.group] > 3; });
  }
  if (options.skip > 0)
    out.erase(out.begin(), out.begin() + std::min<std::ptrdiff_t>(options.skip, static_cast<std::ptrdiff_t>(out.size())));
  if (options.limit > 0 && out.size() > static_cast<size_t>(options.limit))
    out.resize(static_cast<size_t>(options.limit));
  return out;
}

struct OfferFilter
{
  std::string_view prefixes;
  std::string_view name;
};

bool offeredByPrefix(const OfferFilter& filter)
{
  const std::string_view prefixes = filter.prefixes;
  const std::string_view name = filter.name;
  size_t start = 0;
  while (start <= prefixes.size()) {
    const size_t end = prefixes.find(',', start);
    const std::string_view prefix = prefixes.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!prefix.empty() && name.starts_with(prefix))
      return true;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return false;
}

std::string describe(const eval::CaseRun& run)
{
  std::string out = run.item.id + "\t" + run.item.script.front() + "\t";
  for (const auto& turn : run.turns) {
    for (const auto& call : turn.executed)
      out += call.tool + " ";
    out += "| offered: ";
    for (const auto& call : turn.offered)
      out += call.tool + " ";
    out += "| " + turn.reply + " || ";
  }
  std::ranges::replace(out, '\n', ' ');
  return out;
}

}

std::vector<eval::CaseRun> savedRuns(const Options& options, const std::vector<eval::EvalCase>& cases)
{
  std::map<std::string, eval::EvalCase> byId;
  for (const auto& item : cases)
    byId[item.id] = item;
  std::map<std::string, eval::CaseRun> merged;
  size_t start = 0;
  while (start <= options.score.size()) {
    const size_t end = options.score.find(',', start);
    std::ifstream in(options.score.substr(start, end == std::string::npos ? std::string::npos : end - start));
    std::string line;
    while (std::getline(in, line)) {
      const auto parsed = eval::runLineFrom(line);
      if (parsed && byId.contains(parsed->id))
        merged[parsed->id] = {.item = byId[parsed->id], .turns = parsed->turns};
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  std::vector<eval::CaseRun> out;
  out.reserve(merged.size());
  for (auto& [id, run] : merged)
    out.push_back(std::move(run));
  return out;
}

int finish(const Options& options, const std::vector<eval::CaseRun>& runs)
{
  std::string error;
  const eval::ScoreConfig scoring = eval::loadScoreConfig(options.gates, error);
  const auto gates = eval::loadGates(options.gates, "llm");
  if (!gates.error.empty() || !error.empty()) {
    std::printf("[ERROR] %s%s\n", gates.error.c_str(), error.c_str());
    return 1;
  }
  const eval::Metrics metrics = eval::aggregate(runs, scoring);
  const eval::Verdict verdict = eval::check(gates.gates, metrics);
  eval::printMetrics(metrics);
  if (!options.report.empty() && !eval::writeReport(options.report, {.section = "llm", .metrics = metrics, .verdict = verdict}))
    std::printf("[ERROR] cannot write %s\n", options.report.c_str());
  std::fflush(stdout);
  if (!verdict.passed()) {
    std::printf("GATE FAILED\n");
    for (const auto& failure : verdict.failures)
      std::printf("  %s\n", failure.c_str());
    return 1;
  }
  std::printf("GATE PASSED\n");
  return 0;
}

int main(int argc, char** argv)
{
  const Options options = parseOptions(argc, argv);
  if (!options.score.empty()) {
    const auto saved = eval::loadCases(options.cases);
    if (!saved.error.empty()) {
      std::printf("[ERROR] %s\n", saved.error.c_str());
      return 1;
    }
    return finish(options, savedRuns(options, saved.cases));
  }
#ifndef NDEBUG
  if (!options.force) {
    std::printf("[SKIPPED] a debug build decodes about twenty times slower; run the prod build or pass --force\n");
    return kSkipped;
  }
#endif
  if (!std::filesystem::exists(options.llmModel)) {
    std::printf("[SKIPPED] no LLM weights at %s; the LLM-tier gate did not run\n", options.llmModel.c_str());
    return kSkipped;
  }
  if (!options.noFastTier && !std::filesystem::exists(options.intentModel)) {
    std::printf("[SKIPPED] no intent model at %s; the system tier cannot be measured\n", options.intentModel.c_str());
    return kSkipped;
  }
  const auto loaded = eval::loadCases(options.cases);
  if (!loaded.error.empty()) {
    std::printf("[ERROR] %s\n", loaded.error.c_str());
    return 1;
  }
  const ModuleFlags catalog = catalogFlags(options.catalog);
  if (catalog.empty()) {
    std::printf("[ERROR] no module catalog at %s\n", options.catalog.c_str());
    return 1;
  }

  const std::string scratch = writeScratchConfig(options.llmModel);
  ConfigService::load(scratch);
  std::filesystem::remove(scratch);
  ConfigService::setRuntimeString("intent.model_file", options.noFastTier ? "/nonexistent/intent.bin" : options.intentModel);
  llama_backend_init();

  LlmController controller;
  controller.initEngine();
  if (!controller.isEngineLoaded()) {
    std::printf("[ERROR] the LLM did not load from %s\n", options.llmModel.c_str());
    return 1;
  }

  const eval::StubInput stubs{.recorder = std::make_shared<eval::Recorder>(),
                              .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()};
  for (auto& descriptor : eval::stubTools(stubs)) {
    if (!options.offer.empty() && !offeredByPrefix({.prefixes = options.offer, .name = descriptor.spec.name}))
      continue;
    ToolRegistry::instance().registerTool(std::move(descriptor));
  }

  if (options.verbose) {
    moduleGate().apply(catalog);
    std::string names;
    for (const auto& tool : controller.adapter().executor().offered({.role = UserRole::Owner, .modules = moduleGate().snapshot()}))
      names += tool->spec.name + " ";
    std::printf("offered to the owner: %s\n", names.c_str());
    std::printf("declarations: %s\n",
                LfmAdapter::buildToolDeclarations(controller.adapter().executor().offered(
                                                      {.role = UserRole::Owner, .modules = moduleGate().snapshot()}))
                    .substr(0, 1500)
                    .c_str());
  }

  const std::vector<eval::EvalCase> cases = selected(loaded.cases, options);
  std::printf("llm tier: %zu cases of %zu\n", cases.size(), loaded.cases.size());
  std::vector<eval::CaseRun> runs;
  std::ofstream dump;
  if (!options.dump.empty())
    dump.open(options.dump);
  std::ofstream runsOut;
  if (!options.runsOut.empty())
    runsOut.open(options.runsOut, std::ios::app);
  for (const auto& item : cases) {
    moduleGate().apply(flagsFor(catalog, item));
    runs.push_back(runCase(controller, item, stubs));
    if (dump)
      dump << describe(runs.back()) << "\n";
    if (runsOut) {
      Json::StreamWriterBuilder builder;
      builder["indentation"] = "";
      runsOut << Json::writeString(builder, eval::toJson(runs.back())) << "\n";
      runsOut.flush();
    }
    if (options.verbose)
      std::printf("%s\n", describe(runs.back()).c_str());
  }

  controller.shutdownEngine();
  llama_backend_free();
  return finish(options, runs);
}