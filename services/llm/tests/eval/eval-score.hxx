#pragma once

#include "eval-case.hxx"
#include "eval-report.hxx"

#include <json/value.h>

#include <optional>

#include <cstdint>
#include <string>
#include <vector>

namespace eval
{

struct RecordedCall
{
  std::string tool;
  Json::Value arguments;
};

struct TurnResult
{
  std::string reply{};
  std::vector<RecordedCall> executed{};
  std::vector<RecordedCall> offered{};
  std::vector<std::string> previews{};
  std::vector<std::string> confirmed{};
  int64_t ms = 0;
};

struct CaseRun
{
  EvalCase item;
  std::vector<TurnResult> turns;
};

struct ScoreConfig
{
  std::vector<std::string> writeTools;
  std::vector<std::string> ownerOfferMarkers;
  std::vector<std::string> memberOfferMarkers;
  std::vector<std::string> inactiveMarkers;
  std::vector<std::string> questionMarkers;
};

struct ArgumentProbe
{
  const ArgMatcher& matcher;
  const Json::Value& arguments;
};

std::string fold(const std::string& text);

bool matchesArguments(const ArgumentProbe& probe);

Metrics aggregate(const std::vector<CaseRun>& runs, const ScoreConfig& config);

ScoreConfig loadScoreConfig(const std::string& gatesPath, std::string& error);

Json::Value toJson(const CaseRun& run);

struct RunLine
{
  std::string id;
  std::vector<TurnResult> turns;
};

std::optional<RunLine> runLineFrom(const std::string& line);

}
