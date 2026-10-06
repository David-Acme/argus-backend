#pragma once

#include "module-command-lexicon.hxx"

#include <auth/module-snapshot.hxx>

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace module_command
{

struct Input
{
  std::string_view utterance;
  const ModuleSnapshot& modules;
};

struct Command
{
  std::string tool;
  Json::Value arguments{Json::objectValue};
  std::vector<std::string> fill;
};

[[nodiscard]] std::optional<Command> commandFor(const Input& input);

[[nodiscard]] std::optional<std::string> moduleNamedIn(const Input& input);

[[nodiscard]] bool declines(std::string_view utterance);

[[nodiscard]] std::vector<std::string> tokensOf(std::string_view text);

[[nodiscard]] std::size_t phraseLength(Group group, const std::vector<std::string>& words, std::size_t at);

}
