#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <vector>

namespace eval
{

struct ArgMatcher
{
  std::string name;
  std::string equals;
  std::vector<std::string> anyNeedles;
  std::vector<std::string> allNeedles;
  std::string pattern;
};

struct ExpectedCall
{
  std::string tool;
  std::vector<ArgMatcher> args;
};

struct InactiveExpectation
{
  std::string module;
  std::string attempted;
  std::string audience;
};

struct ConfirmExpectation
{
  std::string tool;
  std::string phase;
  std::vector<ArgMatcher> args;
};

struct OfferAccept
{
  std::string module;
  std::string tool;
  std::string attempted;
};

struct EvalCase
{
  std::string id;
  std::string group;
  std::string lang;
  std::string variant;
  std::string role;
  std::vector<std::string> modules;
  std::vector<std::string> script;
  std::string route;
  std::string twin;
  std::vector<ExpectedCall> calls;
  std::vector<std::string> allowed;
  std::optional<InactiveExpectation> inactive;
  std::optional<ConfirmExpectation> confirm;
  std::optional<OfferAccept> offerAccept;
  bool offerDecline = false;
  std::string offerDeclineModule;

  [[nodiscard]] const std::string& utterance() const { return script.front(); }
};

struct LoadedCases
{
  std::vector<EvalCase> cases;
  std::string error;
};

LoadedCases loadCases(const std::string& path);

}
