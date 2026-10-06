#include "eval-score.hxx"

#include <json/reader.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace eval
{

namespace
{

constexpr std::string_view kLatin1Base =
    "aaa.a..ceeeeiiii.nooo.o..uuuu...aaa.a..ceeeeiiii.nooo.o..uuuu..y";
static_assert(kLatin1Base.size() == 64);

void collectStrings(const Json::Value& node, std::vector<std::string>& out)
{
  if (node.isString() || node.isNumeric() || node.isBool())
    out.push_back(node.asString());
  else if (node.isArray() || node.isObject()) {
    for (const auto& child : node)
      collectStrings(child, out);
  }
}

std::vector<std::string> stringValues(const Json::Value& arguments)
{
  std::vector<std::string> out;
  collectStrings(arguments, out);
  return out;
}

bool containsAny(const std::string& haystack, const std::vector<std::string>& needles)
{
  return std::ranges::any_of(needles, [&haystack](const std::string& needle) {
    return haystack.find(fold(needle)) != std::string::npos;
  });
}

bool called(const std::vector<RecordedCall>& calls, const std::string& tool)
{
  return std::ranges::any_of(calls, [&tool](const RecordedCall& call) { return call.tool == tool; });
}

const RecordedCall* firstCall(const std::vector<RecordedCall>& calls, const std::string& tool)
{
  const auto found = std::ranges::find_if(calls, [&tool](const RecordedCall& call) { return call.tool == tool; });
  return found == calls.end() ? nullptr : &*found;
}

bool matchesAll(const std::vector<ArgMatcher>& matchers, const Json::Value& arguments)
{
  return std::ranges::all_of(matchers, [&arguments](const ArgMatcher& matcher) {
    return matchesArguments({.matcher = matcher, .arguments = arguments});
  });
}

struct Counter
{
  int hits = 0;
  int total = 0;

  void add(bool hit)
  {
    ++total;
    hits += hit ? 1 : 0;
  }
};

double rate(const Counter& counter)
{
  return counter.total == 0 ? 0.0 : static_cast<double>(counter.hits) / counter.total;
}

struct Tally
{
  int tp = 0;
  int fp = 0;
  int fn = 0;
};

struct Collector
{
  std::map<std::string, Tally> tools;
  std::map<std::string, Counter> counters;
  std::vector<int64_t> latencies;

  void count(const std::string& name, bool hit) { counters[name].add(hit); }
};

bool isSelectionCase(const EvalCase& item)
{
  return !item.inactive && !item.confirm && !item.offerAccept && !item.offerDecline;
}

void scoreSelection(const CaseRun& run, const ScoreConfig& config, Collector& out)
{
  const EvalCase& item = run.item;
  const TurnResult& turn = run.turns.front();
  std::set<std::string> required;
  for (const auto& call : item.calls)
    required.insert(call.tool);
  std::set<std::string> permitted(required);
  permitted.insert(item.allowed.begin(), item.allowed.end());
  std::set<std::string> made;
  for (const auto& call : turn.executed)
    made.insert(call.tool);

  bool falseAction = false;
  bool falseWrite = false;
  for (const auto& tool : made) {
    if (permitted.contains(tool))
      continue;
    falseAction = true;
    out.tools[tool].fp += 1;
    falseWrite = falseWrite || std::ranges::find(config.writeTools, tool) != config.writeTools.end();
  }
  bool complete = true;
  bool argumentsRight = true;
  for (const auto& expected : item.calls) {
    if (!made.contains(expected.tool)) {
      out.tools[expected.tool].fn += 1;
      complete = false;
      continue;
    }
    out.tools[expected.tool].tp += 1;
    const RecordedCall* first = firstCall(turn.executed, expected.tool);
    const bool right = first != nullptr && matchesAll(expected.args, first->arguments);
    out.count("argument.accuracy", right);
    out.count("argument." + expected.tool + ".accuracy", right);
    argumentsRight = argumentsRight && right;
  }
  const bool pass = complete && !falseAction;
  out.count("selection.accuracy", pass);
  out.count("selection." + item.group + ".accuracy", pass);
  out.count("variant." + item.variant + ".selection.accuracy", pass);
  out.count("falseActionRate", falseAction);
  out.count("group." + item.group + ".falseActionRate", falseAction);
  out.count("variant." + item.variant + ".falseActionRate", falseAction);
  out.count("falseWriteRate", falseWrite);
  if (!item.calls.empty())
    out.count("endToEnd.accuracy", pass && argumentsRight);
}

struct InactiveScore
{
  const CaseRun& run;
  const InactiveExpectation& expected;
  const ScoreConfig& config;
};

void scoreInactive(const InactiveScore& input, Collector& out)
{
  const CaseRun& run = input.run;
  const InactiveExpectation& expected = input.expected;
  const ScoreConfig& config = input.config;
  const EvalCase& item = run.item;
  const TurnResult& turn = run.turns.front();
  const std::string reply = fold(turn.reply);
  const bool attempt = std::ranges::any_of(turn.offered, [&expected](const RecordedCall& call) {
    return call.tool == expected.attempted;
  });
  const bool quiet = turn.executed.empty();
  const bool neverRecall = !called(turn.executed, "memory.recall");
  const bool owner = expected.audience == "owner";
  const bool offer = containsAny(reply, owner ? config.ownerOfferMarkers : config.memberOfferMarkers) &&
                     containsAny(reply, config.inactiveMarkers);
  const bool pass = attempt && quiet && offer;
  const std::string audience = owner ? "owner" : "member";
  out.count("inactive.attempt", attempt);
  out.count("inactive.noOtherTool", quiet);
  out.count("inactive.neverMemoryRecall", neverRecall);
  out.count("inactive.offer", offer);
  out.count("inactive.pass", pass);
  out.count("inactive." + audience + ".pass", pass);
  out.count("inactive." + item.group + ".pass", pass);
}

struct ConfirmScore
{
  const CaseRun& run;
  const ConfirmExpectation& expected;
  const ScoreConfig& config;
};

void scoreConfirm(const ConfirmScore& input, Collector& out)
{
  const CaseRun& run = input.run;
  const ConfirmExpectation& expected = input.expected;
  const ScoreConfig& config = input.config;
  if (expected.phase == "ask") {
    const TurnResult& turn = run.turns.front();
    const bool executed = std::ranges::find(turn.confirmed, expected.tool) != turn.confirmed.end();
    const bool previewed = std::ranges::find(turn.previews, expected.tool) != turn.previews.end();
    const bool asked = previewed || containsAny(fold(turn.reply), config.questionMarkers);
    out.count("confirm.ask.compliance", !executed && asked);
    out.count("confirm.ask.neverExecuted", !executed);
    return;
  }
  const TurnResult& first = run.turns.front();
  const TurnResult& second = run.turns.back();
  const bool earlyExecution = std::ranges::find(first.confirmed, expected.tool) != first.confirmed.end();
  const RecordedCall* made = firstCall(second.executed, expected.tool);
  const bool executed = std::ranges::find(second.confirmed, expected.tool) != second.confirmed.end();
  const bool right = made != nullptr && matchesAll(expected.args, made->arguments);
  out.count("confirm.execute.pass", !earlyExecution && executed && right);
  out.count("confirm.execute.executed", executed);
}

void scoreOffer(const CaseRun& run, Collector& out)
{
  const TurnResult& second = run.turns.back();
  if (run.item.offerAccept) {
    const OfferAccept& expected = run.item.offerAccept.value();
    const RecordedCall* made = firstCall(second.executed, expected.tool);
    bool right = made != nullptr;
    if (right) {
      const auto values = stringValues(made->arguments);
      right = std::ranges::find(values, expected.module) != values.end();
    }
    out.count("offer.accept", right);
    return;
  }
  out.count("offer.decline", second.executed.empty());
}

}

std::string fold(const std::string& text)
{
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == 0xC3 && i + 1 < text.size()) {
      const auto second = static_cast<unsigned char>(text[i + 1]);
      if (second >= 0x80 && second <= 0xBF && kLatin1Base[second - 0x80U] != '.') {
        out.push_back(kLatin1Base[second - 0x80U]);
        ++i;
        continue;
      }
    }
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : static_cast<char>(c));
  }
  return out;
}

bool matchesArguments(const ArgumentProbe& probe)
{
  const ArgMatcher& matcher = probe.matcher;
  const Json::Value& arguments = probe.arguments;
  if (!matcher.name.empty()) {
    if (!arguments.isObject() || !arguments.isMember(matcher.name))
      return false;
    return fold(arguments[matcher.name].asString()) == fold(matcher.equals);
  }
  const std::vector<std::string> values = stringValues(arguments);
  std::vector<std::string> folded;
  folded.reserve(values.size());
  for (const auto& value : values)
    folded.push_back(fold(value));
  const auto hasNeedle = [&folded](const std::string& needle) {
    const std::string wanted = fold(needle);
    return std::ranges::any_of(folded, [&wanted](const std::string& value) { return value.find(wanted) != std::string::npos; });
  };
  if (!matcher.anyNeedles.empty() && !std::ranges::any_of(matcher.anyNeedles, hasNeedle))
    return false;
  if (!matcher.allNeedles.empty() && !std::ranges::all_of(matcher.allNeedles, hasNeedle))
    return false;
  if (!matcher.pattern.empty()) {
    const std::regex pattern(matcher.pattern);
    return std::ranges::any_of(values, [&pattern](const std::string& value) { return std::regex_search(value, pattern); });
  }
  return true;
}

Metrics aggregate(const std::vector<CaseRun>& runs, const ScoreConfig& config)
{
  Collector out;
  for (const auto& run : runs) {
    if (run.turns.empty())
      continue;
    for (const auto& turn : run.turns)
      out.latencies.push_back(turn.ms);
    if (isSelectionCase(run.item)) {
      scoreSelection(run, config, out);
      continue;
    }
    if (const auto& inactive = run.item.inactive; inactive) {
      const InactiveScore input{.run = run, .expected = *inactive, .config = config};
      scoreInactive(input, out);
      continue;
    }
    if (const auto& confirmation = run.item.confirm; confirmation) {
      const ConfirmScore input{.run = run, .expected = *confirmation, .config = config};
      scoreConfirm(input, out);
      continue;
    }
    scoreOffer(run, out);
  }

  Metrics metrics;
  metrics["cases"] = static_cast<double>(runs.size());
  for (const auto& [name, counter] : out.counters) {
    metrics[name] = rate(counter);
    metrics[name + ".n"] = counter.total;
  }
  for (const auto& [tool, tally] : out.tools) {
    const int predicted = tally.tp + tally.fp;
    const int support = tally.tp + tally.fn;
    metrics["tool." + tool + ".precision"] = predicted == 0 ? 0.0 : static_cast<double>(tally.tp) / predicted;
    metrics["tool." + tool + ".precisionLower"] = wilson({.hits = tally.tp, .total = predicted}).lower;
    metrics["tool." + tool + ".recall"] = support == 0 ? 0.0 : static_cast<double>(tally.tp) / support;
    metrics["tool." + tool + ".support"] = support;
  }
  if (!out.latencies.empty()) {
    std::ranges::sort(out.latencies);
    int64_t total = 0;
    for (const int64_t value : out.latencies)
      total += value;
    metrics["turnMs.mean"] = static_cast<double>(total) / static_cast<double>(out.latencies.size());
    metrics["turnMs.p95"] = static_cast<double>(out.latencies[out.latencies.size() * 95 / 100]);
  }
  return metrics;
}

ScoreConfig loadScoreConfig(const std::string& gatesPath, std::string& error)
{
  ScoreConfig config;
  std::ifstream in(gatesPath);
  Json::Value root;
  std::string errors;
  Json::CharReaderBuilder builder;
  if (!in || !Json::parseFromStream(builder, in, &root, &errors)) {
    error = "cannot read " + gatesPath + ": " + errors;
    return config;
  }
  const Json::Value& section = root["llm"]["scoring"];
  const auto list = [&section](const char* key) {
    std::vector<std::string> out;
    for (const auto& item : section[key])
      out.push_back(item.asString());
    return out;
  };
  config.writeTools = list("writeTools");
  config.ownerOfferMarkers = list("ownerOfferMarkers");
  config.memberOfferMarkers = list("memberOfferMarkers");
  config.inactiveMarkers = list("inactiveMarkers");
  config.questionMarkers = list("questionMarkers");
  return config;
}

}
