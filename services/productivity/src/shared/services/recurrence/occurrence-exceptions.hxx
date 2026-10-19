#pragma once

#include "recurrence.hxx"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct OccurrenceException
{
  int64_t occurrence{0};
  bool cancelled{false};
  std::optional<int64_t> startsAt;
  std::optional<int64_t> endsAt;
  std::optional<std::string> title;
  std::optional<std::string> location;

  [[nodiscard]] bool restores() const;
};

using OccurrenceExceptions = std::vector<OccurrenceException>;

struct OccurrenceSlot
{
  int64_t index{0};
  std::optional<int64_t> previous;
  std::optional<int64_t> next;
};

struct NextFiringInput
{
  RecurrenceRule rule;
  int64_t anchor{0};
  int64_t after{0};
  int64_t moment{0};
  const OccurrenceExceptions& exceptions;
};

struct NextFiring
{
  int64_t occurrence{0};
  int64_t at{0};
};

inline constexpr std::size_t kMaxOccurrenceExceptions = 366;
inline constexpr std::string_view kNoOccurrenceExceptions = "[]";

[[nodiscard]] OccurrenceExceptions parseOccurrenceExceptions(std::string_view text);
[[nodiscard]] std::string serializeOccurrenceExceptions(const OccurrenceExceptions& exceptions);
[[nodiscard]] std::optional<OccurrenceSlot> occurrenceSlot(const FirstOccurrenceInput& input);
[[nodiscard]] std::optional<OccurrenceException> exceptionAt(const OccurrenceExceptions& exceptions,
                                                             int64_t occurrence);
[[nodiscard]] int64_t scheduledOccurrence(const OccurrenceExceptions& exceptions, int64_t scheduledAt);
[[nodiscard]] OccurrenceExceptions withException(OccurrenceExceptions exceptions,
                                                 const OccurrenceException& change);
[[nodiscard]] OccurrenceExceptions exceptionsFrom(OccurrenceExceptions exceptions, int64_t occurrence);
[[nodiscard]] std::optional<NextFiring> nextFiring(const NextFiringInput& input);
