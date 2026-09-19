#pragma once

#include <json/value.h>
#include <shared/services/extract/extract-contracts.hxx>
#include <shared/services/extract/extraction-service.hxx>
#include <shared/services/extract/lexicon-extractor.hxx>
#include <shared/services/extract/temporal-resolver.hxx>
#include <string>
#include <vector>

class TieredExtractor : public extract::IFactExtractor
{
public:
  explicit TieredExtractor(ExtractionService& model);

  void rebuild(const std::vector<extract::LexiconEntry>& entries);

  bool extract(const extract::ExtractInput& input,
               std::vector<extract::ExtractedFact>& out) const override;

  LexiconExtractor& lexicon() { return lexicon_; }

private:
  std::vector<extract::ExtractedFact>
  addModelFacts(const extract::ExtractInput& input,
                const Json::Value& root) const;

  LexiconExtractor lexicon_;
  ExtractionService& model_;
  TemporalResolver temporal_;
};
