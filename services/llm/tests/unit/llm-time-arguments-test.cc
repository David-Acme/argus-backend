#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/time-arguments.hxx>
#include <mcp/schema.hxx>
#include <text/iso-time.hxx>

#include <cstdlib>
#include <ctime>
#include <string>

namespace
{
namespace schema = argus::mcp::schema;

struct Moment
{
  int day{0};
  int hour{0};
  int minute{0};
};

int64_t utc(const Moment& moment)
{
  std::tm at{};
  at.tm_year = 2026 - 1900;
  at.tm_mon = 9;
  at.tm_mday = moment.day;
  at.tm_hour = moment.hour;
  at.tm_min = moment.minute;
  return static_cast<int64_t>(timegm(&at));
}

int64_t now()
{
  setenv("TZ", "UTC", 1);
  tzset();
  return utc({.day = 7, .hour = 15, .minute = 20});
}

argus::mcp::ToolSpec eventSpec()
{
  Json::Value starts = schema::text();
  starts["format"] = "date-time";
  Json::Value ends = schema::text();
  ends["format"] = "date-time";
  return {.name = "calendar.create_event",
          .title = "",
          .description = "",
          .inputSchema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                         {.name = "starts_at", .schema = starts, .required = true},
                                         {.name = "ends_at", .schema = ends, .required = false}}),
          .annotations = {},
          .module = "productivity",
          .capability = "agenda.write"};
}

struct EventCall
{
  std::string startsAt;
  std::string utterance;
};

tools::ToolCall callWith(const EventCall& event)
{
  tools::ToolCall call;
  call.name = "calendar.create_event";
  call.arguments["title"] = "reunión";
  if (!event.startsAt.empty())
    call.arguments["starts_at"] = event.startsAt;
  call.context.utterance = event.utterance;
  call.context.lang = "es";
  return call;
}
}

TEST_CASE("a tool with a date-time argument asks for the clock line and others do not")
{
  CHECK(time_arguments::needsClock(eventSpec()));
  CHECK_FALSE(time_arguments::needsClock({.name = "x",
                                          .title = "",
                                          .description = "",
                                          .inputSchema = schema::emptyObject(),
                                          .annotations = {},
                                          .module = "",
                                          .capability = ""}));
  now();
  CHECK(time_arguments::clockLine(utc({.day = 7, .hour = 15, .minute = 20}), "es") == "(Fecha y hora actuales: 2026-10-07T15:20:00+00:00, miércoles)");
  CHECK(time_arguments::clockLine(utc({.day = 7, .hour = 15, .minute = 20}), "en") == "(Current date and time: 2026-10-07T15:20:00+00:00, Wednesday)");
}

TEST_CASE("the time the user said wins over the one the model wrote")
{
  const auto spec = eventSpec();
  auto call = callWith({.startsAt = "2026-10-09T15:00:00", .utterance = "agenda una reunión mañana a las nueve de la mañana"});
  time_arguments::normalize({.call = call, .spec = spec, .now = now()});
  CHECK(call.arguments["starts_at"].asString() == "2026-10-08T09:00:00+00:00");
}

TEST_CASE("an ISO the model wrote is kept when the utterance holds no time, and canonicalized")
{
  const auto spec = eventSpec();
  auto call = callWith({.startsAt = "2026-10-12T16:30", .utterance = "agenda una reunión con Pedro el 12 de octubre"});
  time_arguments::normalize({.call = call, .spec = spec, .now = now()});
  CHECK(call.arguments["starts_at"].asString() == "2026-10-12T16:30:00+00:00");
}

TEST_CASE("a natural phrase in the argument is resolved, and a missing required time is taken from the utterance")
{
  const auto spec = eventSpec();
  auto phrase = callWith({.startsAt = "en 20 minutos", .utterance = "agenda la reunión"});
  time_arguments::normalize({.call = phrase, .spec = spec, .now = now()});
  CHECK(phrase.arguments["starts_at"].asString() == iso_time::format(now() + 1200));

  auto missing = callWith({.startsAt = "", .utterance = "agenda la reunión pasado mañana a las 10"});
  time_arguments::normalize({.call = missing, .spec = spec, .now = now()});
  CHECK(missing.arguments["starts_at"].asString() == "2026-10-09T10:00:00+00:00");
}

TEST_CASE("an optional time is never invented, and a time that cannot be understood is left for the owner to refuse")
{
  const auto spec = eventSpec();
  auto call = callWith({.startsAt = "algún día", .utterance = "agenda la reunión algún día"});
  time_arguments::normalize({.call = call, .spec = spec, .now = now()});
  CHECK(call.arguments["starts_at"].asString() == "algún día");
  CHECK_FALSE(call.arguments.isMember("ends_at"));

  auto optional = callWith({.startsAt = "2026-10-08T09:00", .utterance = "reunión mañana a las nueve"});
  time_arguments::normalize({.call = optional, .spec = spec, .now = now()});
  CHECK_FALSE(optional.arguments.isMember("ends_at"));
}
