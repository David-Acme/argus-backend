#include "eval-tools.hxx"

#include <auth/module-snapshot.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>

#include <json/json.h>
#include <text/sha256.hxx>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view kBundleFlag = "--bundle";

std::vector<tools::ToolHandle> offeredOf(const std::vector<tools::ToolHandle>& all, const Json::Value& names)
{
  std::set<std::string> wanted;
  for (const Json::Value& name : names)
    wanted.insert(name.asString());
  std::vector<tools::ToolHandle> offered;
  for (const tools::ToolHandle& handle : all)
    if (wanted.contains(handle->spec.name))
      offered.push_back(handle);
  return offered;
}

}

int main(int argc, char** argv)
{
  std::filesystem::path bundleDir;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == kBundleFlag)
      bundleDir = argv[i + 1];
  if (bundleDir.empty()) {
    std::cerr << "usage: production-decider --bundle <decider bundle directory>\n";
    return 2;
  }
  std::ifstream listing(bundleDir / "sha256", std::ios::binary);
  const std::string listed((std::istreambuf_iterator<char>(listing)), std::istreambuf_iterator<char>());
  const turn::BundleLoader bundle({.dir = bundleDir, .pin = argus::hash::sha256Hex(listed)});
  if (!bundle.valid()) {
    std::cerr << "the decider bundle did not load: " << bundle.error() << "\n";
    return 1;
  }
  ToolRegistry registry;
  for (tools::ToolDescriptor& descriptor : eval::stubTools({.recorder = std::make_shared<eval::Recorder>(),
                                                             .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()}))
    registry.registerTool(std::move(descriptor));
  const std::vector<tools::ToolHandle> all = registry.all();
  const turn::RuleDecider rules;
  const turn::TriggerDecider triggers;
  const turn::LayaDecider laya({.model = turn::openLayaModel({.bundle = bundle, .options = {}, .decode = bundle.decode()}),
                                .fallback = &triggers,
                                .policy = bundle.policy(),
                                .confidence = bundle.confidenceCalibration(),
                                .now = bundle.nowCalibration()});
  if (!laya.model_ready()) {
    std::cerr << "the Laya model in " << bundleDir.string() << " did not open\n";
    return 1;
  }
  const turn::FirstOf stack({&rules, &laya});
  const ModuleSnapshot modules;
  Json::CharReaderBuilder reader;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  std::string line;
  while (std::getline(std::cin, line)) {
    Json::Value request;
    std::string errors;
    std::istringstream stream(line);
    if (!Json::parseFromStream(reader, stream, &request, &errors))
      return 1;
    const std::string text = request["text"].asString();
    const std::string lang = request["lang"].asString();
    const std::vector<tools::ToolHandle> offered = offeredOf(all, request["tools"]);
    const std::optional<turn::Candidate> candidate =
        stack.decide({.utterance = text, .lang = lang, .offered = offered, .modules = modules, .previousAssistant = {}});
    Json::Value answer;
    answer["seq"] = request["seq"];
    answer["tool"] = Json::nullValue;
    answer["confidence"] = 0.0;
    if (candidate) {
      answer["tool"] = candidate->tool;
      answer["confidence"] = candidate->confidence;
      if (candidate->runnerUp) {
        answer["runnerUp"]["tool"] = candidate->runnerUp->tool;
        answer["runnerUp"]["confidence"] = candidate->runnerUp->confidence;
      }
      if (candidate->now)
        answer["now"] = *candidate->now;
    }
    std::cout << Json::writeString(writer, answer) << '\n' << std::flush;
  }
  return 0;
}
