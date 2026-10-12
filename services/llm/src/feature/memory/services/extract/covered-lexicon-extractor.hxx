#pragma once

#include <feature/memory/services/extract/extract-contracts.hxx>
#include <feature/memory/services/extract/lexicon-extractor.hxx>
#include <vector>

class CoveredLexiconExtractor : public extract::IFactExtractor
{
public:
  void rebuild(const std::vector<extract::LexiconEntry>& entries);

  bool extract(const extract::ExtractInput& input,
               std::vector<extract::ExtractedFact>& out) const override;

private:
  LexiconExtractor lexicon_;
};
