#include <config/config-service.hxx>
#include <feature/llm/services/intent-gate.hxx>
#include <feature/llm/services/tools/app-command.hxx>

#include <json/json.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view kModelFlag = "--model";

std::string_view memoryTool(intent::ToolIntent value)
{
  using intent::ToolIntent;
  switch (value) {
    case ToolIntent::MemorySave:
      return "memory.remember";
    case ToolIntent::MemoryRecall:
      return "memory.recall";
    case ToolIntent::ReminderSet:
      return "memory.remind";
    case ToolIntent::MemoryForget:
      return "memory.forget";
    default:
      return "";
  }
}

bool offered(const Json::Value& tools, std::string_view name)
{
  for (const auto& tool : tools)
    if (tool.asString() == name)
      return true;
  return false;
}

}

int main(int argc, char** argv)
{
  std::string model;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == kModelFlag)
      model = argv[i + 1];
  if (model.empty()) {
    std::cerr << "usage: production-decider --model <intent.bin>\n";
    return 2;
  }
  ConfigService::setRuntimeString("intent.model_file", model);
  const IntentGate gate;
  if (!gate.isLoaded()) {
    std::cerr << "the intent model at " << model << " did not load\n";
    return 1;
  }

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
    Json::Value answer;
    answer["seq"] = request["seq"];
    answer["tool"] = Json::nullValue;
    answer["confidence"] = 0.0;
    if (const auto app = appCommandFor(text); app && offered(request["tools"], app->name)) {
      answer["tool"] = app->name;
      answer["confidence"] = 1.0;
    } else {
      const auto decision = gate.router().decide(text, request["lang"].asString());
      const std::string_view tool = decision.confident ? memoryTool(decision.intent) : std::string_view{};
      if (!tool.empty() && offered(request["tools"], tool)) {
        answer["tool"] = std::string(tool);
        answer["confidence"] = decision.fromRules ? 1.0 : static_cast<double>(decision.score);
      }
    }
    std::cout << Json::writeString(writer, answer) << '\n' << std::flush;
  }
  return 0;
}
