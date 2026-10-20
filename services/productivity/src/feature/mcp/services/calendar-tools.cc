#include "calendar-tools.hxx"

#include "tool-support.hxx"

#include <feature/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/calendar-event/services/calendar-event-feature-service.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/services/recurrence/series-window.hxx>
#include <text/name-match.hxx>
#include <text/spoken-time.hxx>

#include <algorithm>
#include <ctime>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace productivity_tools
{

namespace
{
constexpr int64_t kDay = 86400;
constexpr int kDefaultListed = 10;
constexpr int kMostListed = 20;
constexpr int64_t kLookBehind = kDay;
constexpr int64_t kLookAhead = int64_t{5} * 365 * kDay;
constexpr std::string_view kCancelTool = "calendar.cancel_event";

std::string lang(const argus::mcp::ToolInvocation& invocation)
{
  return std::string(argus::mcp::speech::languageOf(invocation));
}

std::string describe(const CalendarEventSchema& event, const argus::mcp::ToolInvocation& invocation)
{
  return "«" + event.title + "», " + spokenWhen({.epoch = event.startsAt, .allDay = event.isAllDay}, lang(invocation));
}

std::string contextLine(const CalendarEventSchema& event, const argus::mcp::ToolInvocation& invocation)
{
  const bool english = argus::mcp::speech::inEnglish(invocation);
  const std::string language = lang(invocation);
  const spoken_time::When when{.epoch = event.startsAt,
                               .now = static_cast<int64_t>(std::time(nullptr)),
                               .lang = language,
                               .day = spoken_time::Day::Relative,
                               .bare = false};
  std::string line = (english ? "Event: «" : "Evento: «") + event.title + "», " +
                     (event.isAllDay ? spoken_time::day(when) : spoken_time::moment(when));
  if (!event.location.empty())
    line += (english ? ", at " : ", en ") + event.location;
  return line + ".";
}

std::string spokenStart(const CalendarEventSchema& event, const argus::mcp::ToolInvocation& invocation)
{
  const std::string language = lang(invocation);
  const spoken_time::When when{.epoch = event.startsAt,
                               .now = static_cast<int64_t>(std::time(nullptr)),
                               .lang = language,
                               .day = spoken_time::Day::Weekday,
                               .bare = true};
  return event.isAllDay ? spoken_time::day(when) : spoken_time::moment(when);
}

Json::Value summary(const CalendarEventSchema& event)
{
  Json::Value out(Json::objectValue);
  out["id"] = event.id;
  out["title"] = event.title;
  out["startsAt"] = event.startsAt;
  if (event.endsAt)
    out["endsAt"] = *event.endsAt;
  out["location"] = event.location;
  out["allDay"] = event.isAllDay;
  return out;
}

struct ListedEvent
{
  CalendarEventSchema event;
  std::optional<int64_t> occurrence;
};

std::vector<ListedEvent> agendaWithin(const std::vector<CalendarEventSchema>& rows, const CalendarEventRangeInput& range)
{
  std::vector<ListedEvent> listed;
  for (const auto& row : rows) {
    const std::optional<std::string_view> rule = row.recurrenceRule;
    const auto occurrences = occurrencesWithin({.rule = rule,
                                                .startsAt = row.startsAt,
                                                .endsAt = row.endsAt,
                                                .exceptions = row.recurrenceExceptions,
                                                .from = range.from,
                                                .to = range.to});
    for (const auto& occurrence : occurrences) {
      ListedEvent entry{.event = row,
                        .occurrence = occurrence.repeats ? std::optional<int64_t>{occurrence.occurrence}
                                                         : std::nullopt};
      entry.event.startsAt = occurrence.startsAt;
      entry.event.endsAt = occurrence.endsAt;
      if (occurrence.title)
        entry.event.title = *occurrence.title;
      if (occurrence.location)
        entry.event.location = *occurrence.location;
      listed.push_back(std::move(entry));
    }
  }
  std::ranges::sort(listed, [](const ListedEvent& left, const ListedEvent& right) {
    if (left.event.startsAt != right.event.startsAt)
      return left.event.startsAt < right.event.startsAt;
    return left.event.id < right.event.id;
  });
  return listed;
}

int limitOf(const Json::Value& arguments)
{
  if (!arguments["limit"].isIntegral())
    return kDefaultListed;
  return std::clamp(arguments["limit"].asInt(), 1, kMostListed);
}

struct Found
{
  std::optional<CalendarEventSchema> event;
  argus::mcp::ToolOutcome failure;
};

drogon::Task<Found> findEvent(const argus::mcp::ToolInvocation& invocation)
{
  const CalendarEventRepository repository;
  const int64_t userId = invocation.caller.userId;
  if (invocation.arguments["event_id"].isIntegral()) {
    auto event = co_await repository.findById(invocation.arguments["event_id"].asInt64());
    if (event && event->ownerId == userId)
      co_return Found{.event = std::move(event), .failure = {}};
    co_return Found{.event = std::nullopt,
                    .failure = argus::mcp::speech::refuse({.invocation = invocation,
                                        .spanish = "No encuentro ese evento en tu agenda.",
                                        .english = "I cannot find that event in your agenda."},
                                       "event_not_found")};
  }
  const std::string asked = invocation.arguments.get("title", "").asString();
  const auto now = static_cast<int64_t>(std::time(nullptr));
  auto events = co_await repository.findByOwnerRange({.ownerId = userId, .from = now - kLookBehind, .to = now + kLookAhead});
  std::vector<std::string> titles;
  titles.reserve(events.size());
  for (const auto& event : events)
    titles.push_back(event.title);
  const auto match = text_norm::matchName(titles, asked);
  if (match.kind == text_norm::NameMatchKind::Exact)
    co_return Found{.event = events.at(match.hits.front()), .failure = {}};
  if (match.kind == text_norm::NameMatchKind::Ambiguous) {
    std::vector<std::string> options;
    options.reserve(match.hits.size());
    for (const size_t index : match.hits)
      options.push_back(describe(events.at(index), invocation));
    co_return Found{.event = std::nullopt,
                    .failure = argus::mcp::speech::refuse({.invocation = invocation,
                                        .spanish = "¿Cuál de estos eventos? " + argus::mcp::speech::joined(options, "es") + ".",
                                        .english = "Which of these events? " + argus::mcp::speech::joined(options, "en") + "."},
                                       "ambiguous_event")};
  }
  co_return Found{.event = std::nullopt,
                  .failure = argus::mcp::speech::refuse({.invocation = invocation,
                                      .spanish = "No encuentro un evento llamado " + asked + " en tu agenda.",
                                      .english = "I cannot find an event called " + asked + " in your agenda."},
                                     "event_not_found")};
}
}

drogon::Task<argus::mcp::ToolOutcome> createEvent(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const Json::Value& arguments = invocation.arguments;
  const auto startsAt = timeArgument(arguments, "starts_at");
  if (!startsAt)
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "No entendí el día y la hora del evento. Dímelos otra vez.",
                       .english = "I did not understand the day and time of the event. Tell me again."},
                      "invalid_time");
  Json::Value body(Json::objectValue);
  body["title"] = arguments.get("title", "").asString();
  body["description"] = arguments.get("description", "").asString();
  body["location"] = arguments.get("location", "").asString();
  body["startsAt"] = *startsAt;
  if (const auto endsAt = timeArgument(arguments, "ends_at"))
    body["endsAt"] = *endsAt;
  body["isAllDay"] = arguments.get("all_day", false).asBool();
  CalendarEventSchema row;
  try {
    const auto dto = CreateCalendarEventDto::fromJson(body);
    const CalendarEventFeatureService service;
    row = co_await service.create(dto,
                                  {.ownerId = userId,
                                   .actorId = userId,
                                   .idempotencyKey = idempotencyKey({.scope = "calendar.create_event",
                                                                     .userId = userId,
                                                                     .subject = dto.title,
                                                                     .at = dto.startsAt})});
  }
  catch (const ValidationException& error) {
    co_return invalid(error, invocation);
  }
  argus::mcp::ToolOutcome outcome;
  const std::string start = spokenStart(row, invocation);
  outcome.text = argus::mcp::speech::inEnglish(invocation)
                     ? "«" + row.title + "» is scheduled for " + start + "."
                     : "Quedó agendado «" + row.title + "» para " + start + ".";
  outcome.structured = summary(row);
  outcome.structured["readback"] = start;
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> listEvents(argus::mcp::ToolInvocation invocation)
{
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const Json::Value& arguments = invocation.arguments;
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const int64_t from = timeArgument(arguments, "from").value_or(now);
  const int64_t to = timeArgument(arguments, "to").value_or(from + 7 * kDay);
  const CalendarEventRepository repository;
  const CalendarEventRangeInput range{.ownerId = userId, .from = from, .to = to};
  const auto events = agendaWithin(co_await repository.findVisibleAgenda(range), range);
  const auto limit = static_cast<size_t>(limitOf(arguments));
  argus::mcp::ToolOutcome outcome;
  const std::string window = spokenWhen({.epoch = from, .allDay = true}, lang(invocation)) + " – " +
                            spokenWhen({.epoch = to, .allDay = true}, lang(invocation));
  if (events.empty()) {
    outcome.text = argus::mcp::speech::say({.invocation = invocation,
                        .spanish = "No tienes eventos entre " + window + ".",
                        .english = "You have no events between " + window + "."});
    outcome.structured["events"] = Json::Value(Json::arrayValue);
    co_return outcome;
  }
  std::vector<std::string> spoken;
  Json::Value listed(Json::arrayValue);
  Json::Value lines(Json::arrayValue);
  for (size_t index = 0; index < events.size() && index < limit; ++index) {
    const ListedEvent& item = events[index];
    spoken.push_back(describe(item.event, invocation));
    lines.append(contextLine(item.event, invocation));
    Json::Value entry = summary(item.event);
    entry["shared"] = item.event.ownerId != userId;
    if (item.occurrence)
      entry["occurrence"] = static_cast<Json::Int64>(*item.occurrence);
    listed.append(std::move(entry));
  }
  const size_t more = events.size() > limit ? events.size() - limit : 0;
  const std::string head = argus::mcp::speech::inEnglish(invocation) ? "You have " + std::to_string(events.size()) + (events.size() == 1 ? " event: " : " events: ")
                                               : "Tienes " + std::to_string(events.size()) + (events.size() == 1 ? " evento: " : " eventos: ");
  outcome.text = head + argus::mcp::speech::joined(spoken, lang(invocation)) + ".";
  if (more > 0)
    outcome.text += argus::mcp::speech::inEnglish(invocation) ? " And " + std::to_string(more) + " more." : " Y " + std::to_string(more) + " más.";
  outcome.structured["events"] = std::move(listed);
  outcome.structured["lines"] = std::move(lines);
  co_return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> cancelEvent(CancelRequest request)
{
  const argus::mcp::ToolInvocation& invocation = request.invocation;
  const int64_t userId = invocation.caller.userId;
  if (userId <= 0)
    co_return unknownCaller(invocation);
  const auto found = co_await findEvent(invocation);
  if (!found.event)
    co_return found.failure;
  const CalendarEventSchema& event = *found.event;
  const argus::mcp::ConfirmationKey key{.userId = userId, .tool = std::string(kCancelTool), .target = std::to_string(event.id)};
  const std::string token = invocation.arguments.get("confirmation", "").asString();
  argus::mcp::ToolOutcome outcome;
  if (token.empty()) {
    const std::string code = request.ledger->issue(key);
    outcome.text = argus::mcp::speech::inEnglish(invocation) ? "This would cancel " + describe(event, invocation) + "."
                                                             : "Esto cancelaría " + describe(event, invocation) + ".";
    outcome.structured["needsConfirmation"] = true;
    outcome.structured["confirmation"] = code;
    outcome.structured["eventId"] = event.id;
    co_return outcome;
  }
  if (!request.ledger->consume(key, token))
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "Ese código no vale (ya se usó, venció o es de otro evento). Vuelve a pedirle la confirmación al usuario.",
                       .english = "That code is not valid (used, expired or for another event). Ask the user to confirm again."},
                      "confirmation_invalid");
  const CalendarEventFeatureService service;
  if (!co_await service.remove(event.id, userId))
    co_return argus::mcp::speech::refuse({.invocation = invocation,
                       .spanish = "No pude cancelar ese evento.",
                       .english = "I could not cancel that event."},
                      "cancel_failed");
  outcome.text = (argus::mcp::speech::inEnglish(invocation) ? "Cancelled: " : "Cancelado: ") + describe(event, invocation) + ".";
  outcome.structured["eventId"] = event.id;
  outcome.structured["cancelled"] = true;
  co_return outcome;
}

}
