#pragma once

#include <span>
#include <string_view>

namespace reply_claims
{

struct Lexicon
{
  std::string_view language;
  std::span<const std::string_view> performed;
  std::span<const std::string_view> performative;
  std::span<const std::string_view> determiners;
  std::span<const std::string_view> markers;
  std::span<const std::string_view> negators;
  std::span<const std::string_view> hedges;
  std::span<const std::string_view> requests;
  std::span<const std::string_view> leadingRequests;
  std::span<const std::string_view> opening;
  std::span<const std::string_view> states;
  std::span<const std::string_view> copulas;
  std::span<const std::string_view> calls;
  std::span<const std::string_view> callOffers;
  std::span<const std::string_view> genericOffers;
  std::string_view honest;
  std::string_view nudge;
};

[[nodiscard]] std::span<const Lexicon> lexicons();

[[nodiscard]] const Lexicon& lexiconFor(std::string_view language);

}
