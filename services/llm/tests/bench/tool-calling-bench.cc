#include <chrono>
#include <drogon/drogon.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <config/config-service.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/app-tool-descriptors.hxx>
#include <feature/memory/services/memory/memory-tool-descriptors.hxx>
#include <llm/llm-service.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_BENCH_CASES
#define ARGUS_BENCH_CASES "services/llm/tests/fixtures/tools/check.tsv"
#endif

namespace
{
constexpr const char* kDefaultCases = ARGUS_BENCH_CASES;

LlmService gLlm;

struct Case
{
  std::string label;
  std::string text;
};

std::string exeDir()
{
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0)
    return ".";
  buf[n] = '\0';
  std::string path(buf);
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::vector<Case> loadCases(const std::string& path)
{
  std::vector<Case> cases;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      continue;
    cases.push_back(
        {.label = line.substr(0, tab), .text = line.substr(tab + 1)});
  }
  return cases;
}

long long nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kVoicePersona =
    "You are Argus, a warm, natural home voice assistant for a local security "
    "camera system.\nReply strictly in Spanish, in at most two short "
    "sentences. Your reply is spoken aloud.\nCameras: Garaje, Puerta, Patio.";

struct VoiceTally
{
  int total = 0;
  int expected = 0;
  int memoryWrite = 0;
  int appAction = 0;
  int anyTool = 0;
  long long ttftMs = 0;
};

struct VoicePersona
{
  std::string spanish = kVoicePersona;
  std::string english = kVoicePersona;
  bool rowLanguage = false;
  float temperature = 0.0F;
};

std::string readFile(const std::string& path)
{
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool looksEnglish(const std::string& text)
{
  static const std::vector<std::string> kAccents{"\xc3\xa1", "\xc3\xa9", "\xc3\xad", "\xc3\xb3",
                                                 "\xc3\xba", "\xc3\xb1", "\xc2\xbf", "\xc2\xa1"};
  if (std::ranges::any_of(kAccents, [&text](const std::string& mark) { return text.find(mark) != std::string::npos; }))
    return false;
  static const std::vector<std::string> kWords{"the", "my", "that", "is", "what", "show", "did", "are", "i",
                                               "it", "this", "please", "remember", "save", "note", "you", "do"};
  std::istringstream words(text);
  std::string word;
  while (words >> word) {
    std::string bare;
    for (const char c : word)
      if (std::isalpha(static_cast<unsigned char>(c)) != 0)
        bare.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (std::ranges::find(kWords, bare) != kWords.end())
      return true;
  }
  return false;
}

std::string oneLine(std::string text)
{
  std::ranges::replace(text, '\n', ' ');
  std::ranges::replace(text, '\t', ' ');
  return text;
}

bool expectedFor(const std::string& label, const std::vector<std::string>& ran)
{
  const auto has = [&ran](const char* name) {
    return std::ranges::find(ran, name) != ran.end();
  };
  if (label == "memory_save")
    return has("memory.remember") || has("memory.remind");
  if (label == "camera")
    return has("app.show_camera") ||
           (!has("memory.remember") && !has("memory.remind"));
  return ran.empty();
}

int voiceSuite(const std::vector<Case>& cases, const VoicePersona& persona)
{
  std::vector<std::string> ran;
  for (auto descriptor : memoryToolDescriptors()) {
    descriptor.handler = [&ran](const tools::ToolCall& call) {
      ran.push_back(call.name);
      tools::ToolResult result;
      result.ok = true;
      result.output = call.name == "memory.recall" ? "No tengo nada guardado sobre eso."
                                                   : "Guardado.";
      return result;
    };
    ToolRegistry::instance().registerTool(std::move(descriptor));
  }
  for (auto descriptor : appToolDescriptors())
    ToolRegistry::instance().registerTool(std::move(descriptor));

  LlmController controller;
  controller.initEngine();
  if (!controller.isEngineLoaded()) {
    std::cout << "[skip] LLM model not loaded\n";
    return 0;
  }

  std::map<std::string, VoiceTally> tally;
  for (const auto& c : cases) {
    ran.clear();
    std::vector<std::string> actions;
    long long first = -1;
    const long long t0 = nowMs();
    const bool english = persona.rowLanguage && looksEnglish(c.text);
    ChatRequest request;
    request.messages = {{.role = "system", .content = english ? persona.english : persona.spanish},
                        {.role = "user", .content = c.text}};
    request.temperature = persona.temperature;
    request.userId = 7;
    request.role = UserRole::Owner;
    request.lang = english ? "en" : "es";
    request.clientActions = true;
    request.sessionId = "bench";
    std::string reply;
    controller.chatStreamSync(
        {.request = request,
         .onToken =
             [&first, &reply, t0](const std::string& token, bool done) {
               if (!done && first < 0 && !token.empty())
                 first = nowMs() - t0;
               reply += token;
             },
         .stats = nullptr,
         .cancellation = {},
         .onAction = [&actions, &ran](const ClientAction& action) {
           actions.push_back(action.name);
           ran.push_back(action.name);
         }});
    auto& bucket = tally[c.label];
    ++bucket.total;
    bucket.expected += expectedFor(c.label, ran) ? 1 : 0;
    bucket.memoryWrite += std::ranges::any_of(ran, [](const std::string& name) {
                            return name == "memory.remember" || name == "memory.remind";
                          })
                              ? 1
                              : 0;
    bucket.appAction += actions.empty() ? 0 : 1;
    bucket.anyTool += ran.empty() ? 0 : 1;
    bucket.ttftMs += first < 0 ? nowMs() - t0 : first;
    std::cout << c.label << "\t" << c.text << "\t";
    for (const auto& name : ran)
      std::cout << name << " ";
    std::cout << "\t" << (first < 0 ? nowMs() - t0 : first) << " ms\t"
              << (persona.rowLanguage ? request.lang + "\t" + oneLine(reply) : reply.substr(0, 80)) << "\n";
  }

  std::cout << "\nlabel\ttotal\texpected\tmemory_write\tapp_action\tany_tool\tmean_ttft_ms\n";
  for (const auto& [label, bucket] : tally)
    std::cout << label << "\t" << bucket.total << "\t" << bucket.expected << "\t"
              << bucket.memoryWrite << "\t" << bucket.appAction << "\t"
              << bucket.anyTool << "\t" << bucket.ttftMs / std::max(1, bucket.total)
              << "\n";
  return 0;
}

}

int main(int argc, char** argv)
{
  std::string filter;
  int limit = 0;
  bool verbose = false;
  bool voice = false;
  std::string configPath;
  float temperature = 0.0F;
  std::string checkPath = kDefaultCases;
  VoicePersona persona;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verbose")
      verbose = true;
    else if (arg == "--voice")
      voice = true;
    else if (arg == "--config" && i + 1 < argc)
      configPath = argv[++i];
    else if (arg == "--filter" && i + 1 < argc)
      filter = argv[++i];
    else if (arg == "--limit" && i + 1 < argc)
      limit = std::atoi(argv[++i]);
    else if (arg == "--check" && i + 1 < argc)
      checkPath = argv[++i];
    else if (arg == "--temp" && i + 1 < argc)
      temperature = std::stof(argv[++i]);
    else if (arg == "--persona-es" && i + 1 < argc)
      persona.spanish = readFile(argv[++i]);
    else if (arg == "--persona-en" && i + 1 < argc)
      persona.english = readFile(argv[++i]);
    else if (arg == "--row-lang")
      persona.rowLanguage = true;
    else if (arg == "--help") {
      std::cout << "argus-tool-bench [--verbose] [--voice] [--config <toml>] "
                   "[--filter <label>] [--limit <n>] [--check <path>] [--temp <t>] "
                   "[--row-lang] [--persona-es <file>] [--persona-en <file>]\n";
      return 0;
    }
  }

  const std::string dir = exeDir();
  const std::string root = dir + "/../../..";
  if (configPath.empty() && access((root + "/config.toml").c_str(), F_OK) == 0) {
    if (chdir(root.c_str()) != 0) {
      std::cerr << "chdir failed: " << root << "\n";
      return 1;
    }
  }
  ConfigService::load(configPath.empty() ? std::string("config.toml") : configPath);

  if (voice) {
    auto cases = loadCases(checkPath);
    if (limit > 0 && static_cast<size_t>(limit) < cases.size())
      cases.resize(static_cast<size_t>(limit));
    persona.temperature = temperature;
    return voiceSuite(cases, persona);
  }

  gLlm.init();
  if (!gLlm.isLoaded()) {
    std::cout << "[skip] LLM model not loaded (llm.model_path="
              << ConfigService::getString("llm.model_path") << ")\n";
    return 0;
  }
  std::cout << "[ok] LLM loaded ("
            << ConfigService::getString("llm.model_path")
            << ") gpu_layers=" << ConfigService::getInt("llm.gpu_layers")
            << "\n";

  auto cases = loadCases(checkPath);
  if (!filter.empty()) {
    std::vector<Case> kept;
    for (const auto& c : cases)
      if (c.label == filter)
        kept.push_back(c);
    cases = std::move(kept);
  }
  if (limit > 0 && static_cast<size_t>(limit) < cases.size())
    cases.resize(static_cast<size_t>(limit));
  std::cout << "cases: " << cases.size() << " (" << checkPath
            << (filter.empty() ? "" : ", filter=" + filter) << ")\n";

  ToolRegistry& registry = ToolRegistry::instance();
  tools::ToolDescriptor remember;
  remember.name = "memory.remember";
  remember.accessTable = TableName::Memory;
  remember.accessPermission = RolePermission::Create;
  remember.arguments = {{"text", "string", false, {}, ""},
                        {"subject", "string", false, {}, ""},
                        {"predicate", "string", false, {}, ""},
                        {"value", "string", false, {}, ""},
                        {"type",
                         "enum",
                         false,
                         {"persona", "preference", "schedule", "instruction",
                          "attribute"},
                         ""}};
  remember.handler = [](const tools::ToolCall&) {
    tools::ToolResult r;
    r.ok = true;
    r.output = "ok";
    return r;
  };
  registry.registerTool(remember);

  const std::vector<const tools::ToolDescriptor*> tools = {&remember};
  LfmAdapter adapter(gLlm);
  const std::string systemPrompt =
      "Eres Argus. Si el usuario pide guardar o recordar algo, usa "
      "memory.remember. Si no, responde brevemente.";

  if (verbose) {
    for (const auto& c : cases) {
      std::vector<ChatMessage> history;
      history.push_back({.role = "user", .content = c.text});
      std::vector<ChatMessage> msgs = history;
      msgs.insert(msgs.begin(),
                  {.role = "system",
                   .content = systemPrompt + "\nList of tools: " +
                              LfmAdapter::buildToolDeclarations(tools)});
      std::cout << "=== " << c.label << ": " << c.text << "\n";
      std::cout << "--- prompt ---\n"
                << gLlm.buildPrompt(msgs) << "\n--- raw reply ---\n";
      const ChatRequest req{.messages = msgs,
                            .maxTokens = 512,
                            .temperature = 0.0F,
                            .resetContext = false,
                            .toolsEnabled = true,
                            .stop = {},
                            .grammar = {},
                            .grammarRequired = false,
                            .userId = 0,
                            .role = UserRole::Guest,
                            .lang = {}};
      std::cout << gLlm.chat(req) << "\n---\n";
    }
    return 0;
  }

  int llmHits = 0;
  int llmTotal = 0;
  std::vector<double> latencies;
  std::map<std::string, std::pair<int, int>> llmByLabel;

  for (const auto& c : cases) {
    std::vector<ChatMessage> history;
    history.push_back({.role = "user", .content = c.text});
    const long long t0 = nowMs();
    const auto output = adapter.chatWithTools({.systemPrompt = systemPrompt,
                                               .tools = tools,
                                               .role = UserRole::Resident,
                                               .context = {.userId = 7,
                                                           .lang = "es",
                                                           .sessionId = {},
                                                           .channel = "tool_result",
                                                           .utterance = {}},
                                               .maxHops = 1,
                                               .temperature = temperature},
                                              history);
    latencies.push_back(static_cast<double>(nowMs() - t0));

    bool llmFired = false;
    for (const auto& call : output.executed)
      if (call.name == "memory.remember")
        llmFired = true;

    auto& llmBucket = llmByLabel[c.label];
    llmBucket.second++;
    if (llmFired)
      llmBucket.first++;
  }

  for (const auto& [label, bucket] : llmByLabel) {
    if (label == "memory_save")
      llmTotal = bucket.second, llmHits = bucket.first;
  }
  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies[latencies.size() / 2];
  const double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
  const std::string probe = "hola, como estas?";
  std::vector<ChatMessage> bare{{.role = "system", .content = systemPrompt},
                                {.role = "user", .content = probe}};
  std::vector<ChatMessage> withTools{
      {.role = "system",
       .content = systemPrompt + "\nList of tools: " +
                  LfmAdapter::buildToolDeclarations(tools)},
      {.role = "user", .content = probe}};

  const auto timeFirst = [](std::vector<ChatMessage> msgs) {
    const long long t0 = nowMs();
    gLlm.chat({.messages = std::move(msgs),
               .maxTokens = 1,
               .temperature = 0.0F,
               .resetContext = true,
               .stop = {},
               .grammar = {}});
    return static_cast<double>(nowMs() - t0);
  };
  const double bareMs = timeFirst(bare);
  const double toolsMs = timeFirst(withTools);

  std::cout << "system prompt bytes: bare="
            << bare.front().content.size()
            << " with tools=" << withTools.front().content.size() << "\n";
  std::cout << "first-token latency: bare=" << bareMs << " ms with tools="
            << toolsMs << " ms (" << (bareMs > 0 ? toolsMs / bareMs : 0.0)
            << "x)\n";

  std::cout << "LLM memory_save precision: " << llmHits << "/" << llmTotal
            << "\n";
  std::cout << "LLM tool latency: p50=" << p50 << " ms p95=" << p95 << " ms\n";
  std::cout << "per-label (fired/total):\n";
  for (const auto& [label, bucket] : llmByLabel)
    std::cout << "  " << label << ": " << bucket.first << "/" << bucket.second
              << "\n";

  return 0;
}
