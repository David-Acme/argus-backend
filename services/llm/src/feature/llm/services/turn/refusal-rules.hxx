#pragma once

#include "speech-acts.hxx"

#include <feature/llm/services/tools/tool-access.hxx>

#include <optional>
#include <string>
#include <string_view>

namespace turn::refusal
{

inline constexpr std::string_view kPrivatePerson = speech::refusal_reason::kPrivatePerson;
inline constexpr std::string_view kNoRelay = speech::refusal_reason::kNoRelay;
inline constexpr std::string_view kModuleOff = speech::refusal_reason::kModuleOff;
inline constexpr std::string_view kNoAction = speech::refusal_reason::kNoAction;

struct Ask
{
  std::string_view utterance;
  const ToolAudience& audience;
  std::string_view lang;
};

struct Refusal
{
  std::string reason;
  std::string name;
};

struct PolarityAsk
{
  std::string slot;
  std::string tool;
};

[[nodiscard]] std::optional<Refusal> refusalFor(const Ask& ask);

[[nodiscard]] std::optional<PolarityAsk> polarityAskFor(const Ask& ask);

}
