#include "occurrence-exceptions.hxx"

#include <algorithm>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <utility>

namespace
{
std::optional<int64_t> integerOf(const Json::Value& value)
{
  if (!value.isInt64())
    return std::nullopt;
  return value.asInt64();
}

std::optional<std::string> textOf(const Json::Value& value)
{
  if (!value.isString())
    return std::nullopt;
  return value.asString();
}

std::optional<OccurrenceException> exceptionOf(const Json::Value& item)
{
  if (!item.isObject())
    return std::nullopt;
  const auto occurrence = integerOf(item["occurrence"]);
  if (!occurrence || *occurrence <= 0)
    return std::nullopt;
  OccurrenceException exception{.occurrence = *occurrence,
                                .cancelled = item["cancelled"].isBool() && item["cancelled"].asBool(),
                                .startsAt = integerOf(item["startsAt"]),
                                .endsAt = integerOf(item["endsAt"]),
                                .title = textOf(item["title"]),
                                .location = textOf(item["location"])};
  if (exception.cancelled) {
    exception.startsAt.reset();
    exception.endsAt.reset();
    exception.title.reset();
    exception.location.reset();
  }
  if (exception.restores())
    return std::nullopt;
  return exception;
}

Json::Value jsonOf(const OccurrenceException& exception)
{
  Json::Value item(Json::objectValue);
  item["occurrence"] = Json::Int64(exception.occurrence);
  item["cancelled"] = exception.cancelled;
  if (exception.startsAt)
    item["startsAt"] = Json::Int64(*exception.startsAt);
  if (exception.endsAt)
    item["endsAt"] = Json::Int64(*exception.endsAt);
  if (exception.title)
    item["title"] = *exception.title;
  if (exception.location)
    item["location"] = *exception.location;
  return item;
}

void sortByOccurrence(OccurrenceExceptions& exceptions)
{
  std::ranges::sort(exceptions, {}, &OccurrenceException::occurrence);
}
}

bool OccurrenceException::restores() const
{
  return !cancelled && !startsAt && !endsAt && !title && !location;
}

OccurrenceExceptions parseOccurrenceExceptions(std::string_view text)
{
  OccurrenceExceptions exceptions;
  if (text.empty())
    return exceptions;
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  try {
    if (!reader->parse(text.data(), text.data() + text.size(), &root, &errors))
      return exceptions;
  }
  catch (const Json::Exception&) {
    return exceptions;
  }
  if (!root.isArray())
    return exceptions;
  exceptions.reserve(root.size());
  for (const auto& item : root) {
    auto exception = exceptionOf(item);
    if (!exception)
      continue;
    std::erase_if(exceptions, [&exception](const OccurrenceException& known) {
      return known.occurrence == exception->occurrence;
    });
    exceptions.push_back(std::move(*exception));
  }
  sortByOccurrence(exceptions);
  return exceptions;
}

std::string serializeOccurrenceExceptions(const OccurrenceExceptions& exceptions)
{
  Json::Value root(Json::arrayValue);
  for (const auto& exception : exceptions)
    root.append(jsonOf(exception));
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, root);
}

std::optional<OccurrenceSlot> occurrenceSlot(const FirstOccurrenceInput& input)
{
  const auto index = occurrenceIndex(input);
  if (!index)
    return std::nullopt;
  OccurrenceSlot slot{.index = *index, .previous = std::nullopt, .next = std::nullopt};
  if (*index > 0)
    slot.previous = occurrenceAt({.rule = input.rule, .anchor = input.anchor, .index = *index - 1});
  slot.next = occurrenceAt({.rule = input.rule, .anchor = input.anchor, .index = *index + 1});
  return slot;
}

std::optional<OccurrenceException> exceptionAt(const OccurrenceExceptions& exceptions, int64_t occurrence)
{
  const auto found = std::ranges::find(exceptions, occurrence, &OccurrenceException::occurrence);
  if (found == exceptions.end())
    return std::nullopt;
  return *found;
}

int64_t scheduledOccurrence(const OccurrenceExceptions& exceptions, int64_t scheduledAt)
{
  const auto moved = std::ranges::find_if(exceptions, [scheduledAt](const OccurrenceException& exception) {
    return !exception.cancelled && exception.startsAt == scheduledAt;
  });
  return moved == exceptions.end() ? scheduledAt : moved->occurrence;
}

OccurrenceExceptions withException(OccurrenceExceptions exceptions, const OccurrenceException& change)
{
  std::erase_if(exceptions, [&change](const OccurrenceException& known) {
    return known.occurrence == change.occurrence;
  });
  if (!change.restores())
    exceptions.push_back(change);
  sortByOccurrence(exceptions);
  return exceptions;
}

OccurrenceExceptions exceptionsFrom(OccurrenceExceptions exceptions, int64_t occurrence)
{
  std::erase_if(exceptions, [occurrence](const OccurrenceException& known) {
    return known.occurrence < occurrence;
  });
  return exceptions;
}

std::optional<NextFiring> nextFiring(const NextFiringInput& input)
{
  std::optional<NextFiring> behind;
  for (const auto& exception : input.exceptions) {
    if (exception.cancelled || !exception.startsAt)
      continue;
    if (exception.occurrence <= input.after || exception.occurrence > input.moment)
      continue;
    if (*exception.startsAt <= input.moment)
      continue;
    if (!behind || *exception.startsAt < behind->at)
      behind = NextFiring{.occurrence = exception.occurrence, .at = *exception.startsAt};
  }
  if (behind)
    return behind;

  int64_t cursor = std::max(input.after, input.moment);
  for (std::size_t step = 0; step <= input.exceptions.size(); ++step) {
    const auto next = firstOccurrenceAfter({.rule = input.rule, .anchor = input.anchor, .moment = cursor});
    if (!next)
      return std::nullopt;
    const auto exception = exceptionAt(input.exceptions, *next);
    if (!exception || (!exception->cancelled && !exception->startsAt))
      return NextFiring{.occurrence = *next, .at = *next};
    if (!exception->cancelled && *exception->startsAt > input.moment)
      return NextFiring{.occurrence = *next, .at = *exception->startsAt};
    cursor = *next;
  }
  return std::nullopt;
}

std::optional<SnoozedSeries> snoozeSeries(const SnoozeInput& input)
{
  const int64_t current = scheduledOccurrence(input.exceptions, input.scheduledAt);
  const auto slot = occurrenceSlot({.rule = input.rule, .anchor = input.anchor, .moment = current});
  if (!slot)
    return std::nullopt;
  if (!slot->next || input.target < *slot->next) {
    const std::optional<int64_t> movedTo =
        input.target == current ? std::nullopt : std::optional<int64_t>{input.target};
    return SnoozedSeries{.scheduledAt = input.target,
                         .exceptions = withException(input.exceptions,
                                                     {.occurrence = current,
                                                      .cancelled = false,
                                                      .startsAt = movedTo,
                                                      .endsAt = std::nullopt,
                                                      .title = std::nullopt,
                                                      .location = std::nullopt}),
                         .folded = false};
  }
  auto skipped = withException(input.exceptions,
                               {.occurrence = current,
                                .cancelled = true,
                                .startsAt = std::nullopt,
                                .endsAt = std::nullopt,
                                .title = std::nullopt,
                                .location = std::nullopt});
  const auto fire = nextFiring({.rule = input.rule,
                                .anchor = input.anchor,
                                .after = current - 1,
                                .moment = slot->previous.value_or(0),
                                .exceptions = skipped});
  if (!fire)
    return std::nullopt;
  return SnoozedSeries{.scheduledAt = fire->at, .exceptions = std::move(skipped), .folded = true};
}
