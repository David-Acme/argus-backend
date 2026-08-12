#pragma once

#include <cstdint>
#include <shared/enums.hxx>
#include <string>
#include <string_view>
#include <vector>

namespace extract
{

inline constexpr const char* kFactTemplateEs =
    "{\"entidad\": \"\", \"verbo\": \"\", \"complemento\": \"\", "
    "\"cuando\": \"\"}";

inline constexpr const char* kFactSchemaEs =
    "{\"type\": \"object\", \"properties\": {"
    "\"entidad\": {\"type\": \"string\", \"description\": \"el "
    "sintagma nominal completo del que habla la frase, incluido su poseedor, "
    "copiado palabra por palabra\"}, "
    "\"verbo\": {\"type\": \"string\", \"description\": \"solo el "
    "verbo, sin su complemento, copiado palabra por palabra\"}, "
    "\"complemento\": {\"type\": \"string\", \"description\": \"a "
    "qué se aplica el verbo, copiado palabra por palabra, cadena vacía si no "
    "hay\"}, "
    "\"cuando\": {\"type\": \"string\", \"description\": \"la "
    "expresión temporal, copiada palabra por palabra, cadena vacía si la "
    "frase no la tiene\"}}}";

inline constexpr const char* kFactTemplateEn =
    "{\"entity\": \"\", \"verb\": \"\", \"complement\": \"\", "
    "\"when\": \"\"}";

inline constexpr const char* kFactSchemaEn =
    "{\"type\": \"object\", \"properties\": {"
    "\"entity\": {\"type\": \"string\", \"description\": \"the full "
    "noun phrase the sentence is about, including its possessor, copied word "
    "for word\"}, "
    "\"verb\": {\"type\": \"string\", \"description\": \"the verb "
    "only, without its complement, copied word for word\"}, "
    "\"complement\": {\"type\": \"string\", \"description\": \"what "
    "the verb applies to, copied word for word, empty string if absent\"}, "
    "\"when\": {\"type\": \"string\", \"description\": \"the time "
    "expression, copied word for word, empty string if the sentence has "
    "none\"}}}";

inline const char* factTemplateFor(std::string_view lang)
{
  return lang == "en" ? kFactTemplateEn : kFactTemplateEs;
}

inline const char* factSchemaFor(std::string_view lang)
{
  return lang == "en" ? kFactSchemaEn : kFactSchemaEs;
}

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

enum class ExtractTier : uint8_t
{
  Lexicon = 0,
  Model,
  Fallback,
};

struct ExtractInput
{
  std::string_view clause;
  std::string_view lang;
  int64_t userId;
  bool requireModel = false;
  bool allowModel = true;
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
  ExtractTier tier = ExtractTier::Model;
};

class IFactExtractor
{
public:
  virtual ~IFactExtractor() = default;
  virtual bool extract(const ExtractInput& input,
                       std::vector<ExtractedFact>& out) const = 0;
};

} // namespace extract
