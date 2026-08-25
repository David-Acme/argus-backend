#include <chrono>
#include <drogon/drogon.h>
#include <fstream>
#include <iostream>
#include <map>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/intent/intent-service.hxx>
#include <shared/services/llm/lfm-adapter.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

LlmService gLlm;
IntentService gIntent;

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

} // namespace

int main(int argc, char** argv)
{
  std::string filter;
  int limit = 0;
  bool verbose = false;
  std::string checkPath = "labs/intent-data/check.tsv";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verbose")
      verbose = true;
    else if (arg == "--filter" && i + 1 < argc)
      filter = argv[++i];
    else if (arg == "--limit" && i + 1 < argc)
      limit = std::atoi(argv[++i]);
    else if (arg == "--check" && i + 1 < argc)
      checkPath = argv[++i];
    else if (arg == "--help") {
      std::cout << "argus-tool-bench [--verbose] [--filter <label>] "
                   "[--limit <n>] [--check <path>]\n";
      return 0;
    }
  }

  const std::string dir = exeDir();
  const std::string root = dir + "/../../..";
  if (access((root + "/config.toml").c_str(), F_OK) == 0) {
    if (chdir(root.c_str()) != 0) {
      std::cerr << "chdir failed: " << root << "\n";
      return 1;
    }
  }
  ConfigService::load("config.toml");

  gIntent.init();
  gLlm.init();
  if (!gLlm.isLoaded()) {
    std::cout << "[skip] LLM model not loaded (llm.model_path="
              << ConfigService::getString("llm.model_path") << ")\n";
    gIntent.shutdown();
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
  remember.arguments = {{"subject", "string", true, {}, ""},
                        {"predicate", "string", true, {}, ""},
                        {"value", "string", true, {}, ""}};
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
                            .resetContext = false};
      std::cout << gLlm.chat(req) << "\n---\n";
    }
    gIntent.shutdown();
    return 0;
  }

  int llmHits = 0;
  int llmTotal = 0;
  int ftHits = 0;
  int ftTotal = 0;
  std::vector<double> latencies;
  int both = 0;
  int neither = 0;
  int mismatch = 0;
  std::map<std::string, std::pair<int, int>> llmByLabel;
  std::map<std::string, std::pair<int, int>> ftByLabel;

  for (const auto& c : cases) {
    const auto ftHitsNow = gIntent.match(c.text);
    const bool ftFired =
        IntentService::fired(ftHitsNow, ToolIntent::MemorySave);
    auto& ftBucket = ftByLabel[c.label];
    ftBucket.second++;
    if (ftFired)
      ftBucket.first++;

    std::vector<ChatMessage> history;
    history.push_back({.role = "user", .content = c.text});
    const long long t0 = nowMs();
    const auto output = adapter.chatWithTools({.systemPrompt = systemPrompt,
                                               .tools = tools,
                                               .role = UserRole::Resident,
                                               .context = {.userId = 7,
                                                           .lang = "es",
                                                           .sessionId = {}},
                                               .maxHops = 1,
                                               .temperature = -1.0F},
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

    if (llmFired == ftFired)
      (llmFired ? both++ : neither++);
    else
      ++mismatch;
  }

  for (const auto& [label, bucket] : llmByLabel) {
    if (label == "memory_save")
      llmTotal = bucket.second, llmHits = bucket.first;
  }
  for (const auto& [label, bucket] : ftByLabel) {
    if (label == "memory_save")
      ftTotal = bucket.second, ftHits = bucket.first;
  }

  std::sort(latencies.begin(), latencies.end());
  const double p50 = latencies[latencies.size() / 2];
  const double p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
  std::cout << "fastText memory_save precision: " << ftHits << "/" << ftTotal
            << "\n";
  std::cout << "LLM     memory_save precision: " << llmHits << "/" << llmTotal
            << "\n";
  std::cout << "agreement: " << both << " both, " << neither << " neither, "
            << mismatch << " mismatch\n";
  std::cout << "LLM tool latency: p50=" << p50 << " ms p95=" << p95 << " ms\n";
  std::cout << "per-label (fired/total):\n";
  for (const auto& [label, bucket] : llmByLabel)
    std::cout << "  LLM " << label << ": " << bucket.first << "/"
              << bucket.second << " | fastText: " << ftByLabel[label].first
              << "/" << ftByLabel[label].second << "\n";

  gIntent.shutdown();
  return 0;
}
