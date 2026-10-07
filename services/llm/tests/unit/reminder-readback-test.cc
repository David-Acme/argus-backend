#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/memory/services/memory/reminder-readback.hxx>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

namespace
{
struct Moment
{
  int month{10};
  int day{0};
  int hour{0};
  int minute{0};
};

int64_t at(const Moment& moment)
{
  setenv("TZ", "UTC", 1);
  tzset();
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = moment.month - 1;
  local.tm_mday = moment.day;
  local.tm_hour = moment.hour;
  local.tm_min = moment.minute;
  return static_cast<int64_t>(timegm(&local));
}

using reminder_readback::Call;

struct Said
{
  Moment moment;
  std::string_view lang;
  Call call{Call::NotAttempted};
  bool listed{true};
  ReminderCallOutcome why{ReminderCallOutcome::Refused};
};

reminder_readback::Spoken spokenOf(const Said& input)
{
  return {.fireAt = at(input.moment), .now = at({.day = 7, .hour = 15, .minute = 20}), .lang = input.lang, .call = input.call, .listed = input.listed, .why = input.why};
}

std::string said(const Said& input)
{
  return reminder_readback::sentence(spokenOf(input));
}
}

TEST_CASE("a reminder that will ring says the resolved day and time in words, in Spanish and in English")
{
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .call = Call::Scheduled}) == " Te llamaré el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .call = Call::Scheduled}) == " I will call you on Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 7, .hour = 17, .minute = 30}, .lang = "es", .call = Call::Scheduled}) ==
        " Te llamaré el miércoles 7 a las 5:30 de la tarde.");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "es", .call = Call::Scheduled}) ==
        " Te llamaré el jueves 5 de noviembre a las 9 de la mañana.");
  CHECK(said({.moment = {.month = 11, .day = 5, .hour = 9}, .lang = "en", .call = Call::Scheduled}) ==
        " I will call you on Thursday, November 5th at 9 AM.");
}

TEST_CASE("a reminder that is only listed says the same day and time without promising a call")
{
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es"}) == " Quedó en tus recordatorios para el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en"}) == " It is in your reminders for Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 9, .hour = 12}, .lang = "es"}) == " Quedó en tus recordatorios para el viernes 9 a mediodía.");
  CHECK(said({.moment = {.day = 9, .hour = 0}, .lang = "en"}) == " It is in your reminders for Friday the 9th at 12 AM.");
}

TEST_CASE("a call that could not be scheduled is said so, with the day and time the reminder was for, and never as a call")
{
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .call = Call::Failed}) ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .call = Call::Failed}) ==
        " I saved it in your reminders, but I could not schedule the call. It was for Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .call = Call::Failed, .listed = false}) ==
        " Lo guardé, pero no pude programar la llamada. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "en", .call = Call::Failed, .listed = false}) ==
        " I saved it, but I could not schedule the call. It was for Thursday the 8th at 3 PM.");
  CHECK(said({.moment = {.day = 8, .hour = 15}, .lang = "es", .call = Call::Failed}).find("llamaré") == std::string::npos);
}

TEST_CASE("a call the service refused says why, so the user knows what to change")
{
  const auto failed = [](ReminderCallOutcome why, std::string_view lang) {
    return said({.moment = {.day = 8, .hour = 15}, .lang = lang, .call = Call::Failed, .listed = true, .why = why});
  };
  CHECK(failed(ReminderCallOutcome::TooFar, "es") ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. Solo puedo llamarte hasta dentro de 12 meses. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(failed(ReminderCallOutcome::InThePast, "es") ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. Esa hora ya pasó. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(failed(ReminderCallOutcome::TooMany, "es") ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. Ya tienes muchas llamadas pendientes. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(failed(ReminderCallOutcome::Unavailable, "es") ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. El servicio de llamadas no responde ahora. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(failed(ReminderCallOutcome::Refused, "es") ==
        " Lo guardé en tus recordatorios, pero no pude programar la llamada. Era para el jueves 8 a las 3 de la tarde.");
  CHECK(failed(ReminderCallOutcome::TooFar, "en") ==
        " I saved it in your reminders, but I could not schedule the call. I can only call you up to 12 months ahead. It was for Thursday the 8th at 3 PM.");
  CHECK(failed(ReminderCallOutcome::InThePast, "en") ==
        " I saved it in your reminders, but I could not schedule the call. That time has already passed. It was for Thursday the 8th at 3 PM.");
  CHECK(failed(ReminderCallOutcome::TooMany, "en") ==
        " I saved it in your reminders, but I could not schedule the call. You already have a lot of calls waiting. It was for Thursday the 8th at 3 PM.");
  CHECK(failed(ReminderCallOutcome::Unavailable, "en") ==
        " I saved it in your reminders, but I could not schedule the call. The call service is not answering right now. It was for Thursday the 8th at 3 PM.");
}

TEST_CASE("the moment a reply must carry is the day and time with its preposition")
{
  CHECK(reminder_readback::moment(spokenOf({.moment = {.day = 8, .hour = 15}, .lang = "es", .call = Call::Scheduled})) ==
        "el jueves 8 a las 3 de la tarde");
  CHECK(reminder_readback::moment(spokenOf({.moment = {.day = 8, .hour = 15}, .lang = "en", .call = Call::Failed})) ==
        "on Thursday the 8th at 3 PM");
}
