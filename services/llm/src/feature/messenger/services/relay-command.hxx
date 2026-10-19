#pragma once

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay_command
{

struct Command
{
  std::string tool;
  Json::Value arguments{Json::objectValue};
  std::vector<std::string> fill;
  bool reply{false};
};

[[nodiscard]] std::optional<Command> commandFor(std::string_view utterance);

}
