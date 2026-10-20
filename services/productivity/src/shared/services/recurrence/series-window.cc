#include "series-window.hxx"

#include "occurrence-exceptions.hxx"
#include "recurrence.hxx"

#include <algorithm>
#include <utility>

namespace
{
struct ShapeInput
{
  const SeriesWindowInput& series;
  int64_t occurrence{0};
  const OccurrenceException* exception{nullptr};
};

SeriesOccurrence shaped(const ShapeInput& input)
{
  const SeriesWindowInput& series = input.series;
  const OccurrenceException* exception = input.exception;
  const int64_t startsAt = exception && exception->startsAt ? *exception->startsAt : input.occurrence;
  std::optional<int64_t> endsAt;
  if (exception && exception->endsAt)
    endsAt = exception->endsAt;
  else if (series.endsAt)
    endsAt = startsAt + (*series.endsAt - series.startsAt);
  return SeriesOccurrence{.occurrence = input.occurrence,
                          .startsAt = startsAt,
                          .endsAt = endsAt,
                          .title = exception ? exception->title : std::nullopt,
                          .location = exception ? exception->location : std::nullopt,
                          .repeats = true};
}

bool within(int64_t at, const SeriesWindowInput& input)
{
  return at >= input.from && at <= input.to;
}

std::vector<int64_t> startsWithin(const RecurrenceRule& rule, const SeriesWindowInput& input)
{
  std::vector<int64_t> starts;
  if (input.startsAt > input.to)
    return starts;
  std::optional<int64_t> next = input.startsAt >= input.from
                                    ? occurrenceAt({.rule = rule, .anchor = input.startsAt, .index = 0})
                                    : firstOccurrenceAfter(
                                          {.rule = rule, .anchor = input.startsAt, .moment = input.from - 1});
  while (next && *next <= input.to && starts.size() < kMaxSeriesOccurrences) {
    starts.push_back(*next);
    next = firstOccurrenceAfter({.rule = rule, .anchor = input.startsAt, .moment = *next});
  }
  return starts;
}
}

std::vector<SeriesOccurrence> occurrencesWithin(const SeriesWindowInput& input)
{
  std::vector<SeriesOccurrence> found;
  if (input.from > input.to)
    return found;
  const auto rule = input.rule ? parseRecurrenceRule(*input.rule) : std::nullopt;
  if (!rule) {
    if (within(input.startsAt, input))
      found.push_back({.occurrence = input.startsAt,
                       .startsAt = input.startsAt,
                       .endsAt = input.endsAt,
                       .title = std::nullopt,
                       .location = std::nullopt,
                       .repeats = false});
    return found;
  }

  OccurrenceExceptions exceptions = parseOccurrenceExceptions(input.exceptions);
  for (const int64_t occurrence : startsWithin(*rule, input)) {
    const auto matched = std::ranges::find(exceptions, occurrence, &OccurrenceException::occurrence);
    if (matched == exceptions.end()) {
      found.push_back(shaped({.series = input, .occurrence = occurrence, .exception = nullptr}));
      continue;
    }
    const OccurrenceException exception = std::move(*matched);
    exceptions.erase(matched);
    if (exception.cancelled)
      continue;
    SeriesOccurrence entry = shaped({.series = input, .occurrence = occurrence, .exception = &exception});
    if (within(entry.startsAt, input))
      found.push_back(std::move(entry));
  }
  for (const auto& exception : exceptions) {
    if (exception.cancelled || !exception.startsAt || exception.occurrence < input.startsAt)
      continue;
    if (!within(*exception.startsAt, input) || found.size() >= kMaxSeriesOccurrences)
      continue;
    found.push_back(shaped({.series = input, .occurrence = exception.occurrence, .exception = &exception}));
  }
  std::ranges::sort(found, {}, &SeriesOccurrence::startsAt);
  return found;
}
