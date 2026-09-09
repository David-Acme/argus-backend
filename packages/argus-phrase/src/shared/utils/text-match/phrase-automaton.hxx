#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace text_match
{

enum class PatternClass : uint8_t
{
  Entity,
  Predicate,
  Kinship,
  Possessive,
  Connector,
  Stopword,
  FirstPerson,
};

struct PatternRef
{
  PatternClass classId;
  uint32_t payloadId;
  std::string text;
};

struct Match
{
  uint32_t patternIndex;
  uint32_t begin;
  uint32_t end;
};

struct MatchBuffer
{
  std::vector<Match> items;
  void clear() { items.clear(); }
  void reserve(size_t capacity) { items.reserve(capacity); }
};

class PhraseAutomaton
{
public:
  struct Pattern
  {
    PatternClass classId;
    uint16_t length;
    uint32_t payloadId;
  };

  static std::shared_ptr<const PhraseAutomaton>
  build(const std::vector<PatternRef>& patterns);

  void match(std::string_view text, MatchBuffer& out) const;

  size_t patternCount() const { return patterns_.size(); }
  const Pattern& pattern(uint32_t patternIndex) const
  {
    return patterns_[patternIndex];
  }
  size_t bytesUsed() const;

private:
  struct Node
  {
    uint32_t transStart;
    uint32_t outStart;
    uint32_t fail;
    uint16_t transCount;
    uint16_t outCount;
  };

  std::vector<Node> nodes_;
  std::vector<uint8_t> transByte_;
  std::vector<uint32_t> transNext_;
  std::vector<uint32_t> output_;
  std::vector<Pattern> patterns_;
};

} // namespace text_match
