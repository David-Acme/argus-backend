#pragma once

#include "pocket-bundle.hxx"

#include <string>
#include <string_view>

struct PocketPrompt
{
  std::string text;
  int framesAfterEos{0};
};

struct PocketPromptInput
{
  std::string_view text;
  const PocketBundle& bundle;
};

[[nodiscard]] PocketPrompt preparePocketPrompt(const PocketPromptInput& input);
