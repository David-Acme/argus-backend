#pragma once

#include <cstdint>
#include <phrase/lexicon-kind.hxx>
#include <string>
#include <string_view>
#include <vector>

namespace extract
{

enum class TemporalKind : uint8_t
{
  None = 0,
  Date,
  Time,
  Weekday,
  Recurrence,
  Relative,
};

enum class Recurrence : uint8_t
{
  None = 0,
  Daily,
  Weekly,
  Monthly,
};

struct TemporalValue
{
  TemporalKind kind = TemporalKind::None;
  int weekday = -1;
  int minuteOfDay = -1;
  Recurrence recur = Recurrence::None;
  int64_t absolute = 0;
  std::string surface;
};

struct LexiconEntry
{
  LexiconKind kind;
  std::string lang;
  std::string surface;
  std::string canonical;
};

struct ExtractInput
{
  std::string_view clause;
  std::string_view lang;
  int64_t userId;
};

struct ExtractedFact
{
  std::string subject;
  std::string predicate;
  std::string value;
  std::string factType;
  TemporalValue when;
  int64_t subjectEntityId = 0;
  float confidence = 0.0F;
};

class IFactExtractor
{
public:
  virtual ~IFactExtractor() = default;
  virtual bool extract(const ExtractInput& input,
                       std::vector<ExtractedFact>& out) const = 0;
};

}
