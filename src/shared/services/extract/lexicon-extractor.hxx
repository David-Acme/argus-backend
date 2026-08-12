#pragma once

#include <memory>
#include <shared/services/extract/extract-contracts.hxx>
#include <shared/services/extract/temporal-resolver.hxx>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <string>
#include <utility>
#include <vector>

class LexiconExtractor : public extract::IFactExtractor
{
public:
  LexiconExtractor();

  bool extract(const extract::ExtractInput& input,
               std::vector<extract::ExtractedFact>& out) const override;

  void rebuild(const std::vector<extract::LexiconEntry>& entries);

  static std::string normalize(std::string_view text);

private:
  struct PredicateRule
  {
    std::string surface;
    std::string canonical;
  };

  void build(const std::vector<extract::LexiconEntry>& entries);

  std::shared_ptr<const text_match::PhraseAutomaton> automaton_;
  std::vector<PredicateRule> predicates_;
  std::vector<std::string> kinship_;
  std::vector<std::string> firstPerson_;
  std::vector<std::string> stopwords_;
  TemporalResolver temporal_;
};
