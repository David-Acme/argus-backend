#pragma once

#include <cstdint>
#include <string>

enum class LexiconKind : uint8_t
{
  Predicate = 0,
  Kinship,
  FirstPerson,
  Stopword
};

inline std::string lexiconKindToString(LexiconKind k)
{
  switch (k) {
    case LexiconKind::Kinship:
      return "kinship";
    case LexiconKind::FirstPerson:
      return "first_person";
    case LexiconKind::Stopword:
      return "stopword";
    default:
      return "predicate";
  }
}

inline LexiconKind lexiconKindFromString(const std::string& s)
{
  if (s == "kinship")
    return LexiconKind::Kinship;
  if (s == "first_person")
    return LexiconKind::FirstPerson;
  if (s == "stopword")
    return LexiconKind::Stopword;
  return LexiconKind::Predicate;
}
